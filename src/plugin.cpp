#include "logger.h"
#include "papyrus_bridge.h"
#include "screen_manager.h"
#include "screen_channels.h"
#include "screen_controller.h"
#include "video_surface.h"

#include "sfse/PluginAPI.h"
#include "sfse_common/sfse_version.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace
{
Logger g_logger;
std::unique_ptr<ScreenManager> g_screenManager;
std::unique_ptr<PapyrusBridge> g_papyrusBridge;
SurfaceConfig g_surfaceConfig;
VideoConfig g_videoConfig;
bool g_videoEnabled = true;
std::unique_ptr<ScreenController> g_screenController;

std::string trimCopy(std::string value)
{
    auto isSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };

    const auto begin = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto end = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::string toLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parseBool(const std::string& value)
{
    const std::string v = toLowerAscii(value);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::uint32_t parseUInt(const std::string& value, std::uint32_t fallback)
{
    try {
        return static_cast<std::uint32_t>(std::stoul(value, nullptr, 0));
    } catch (...) {
        return fallback;
    }
}

void applySurfaceKey(const std::string& section, const std::string& key, const std::string& value, Logger& logger)
{
    SurfaceConfig& c = g_surfaceConfig;
    if (section == "surface") {
        if (key == "width" || key == "height") {
            logger.info("[Surface] " + key + " is no longer used: screen channel sizes are built in (screen_channels.h).");
        } else if (key == "formats") {
            std::vector<std::uint32_t> formats;
            std::size_t start = 0;
            while (start <= value.size()) {
                const std::size_t comma = value.find(',', start);
                const std::string item = trimCopy(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (!item.empty()) {
                    formats.push_back(parseUInt(item, 0));
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            if (!formats.empty()) {
                c.formats = formats;
            }
        } else {
            logger.warn("Unknown [Surface] key '" + key + "'.");
        }
    } else if (section == "render") {
        if (key == "enabled") {
            c.enabled = parseBool(value);
        } else if (key == "fps") {
            try {
                c.fps = std::clamp(std::stod(value), 1.0, 60.0);
            } catch (...) {
            }
        } else if (key == "simultaneousaccess") {
            c.simultaneousAccess = parseBool(value);
        } else if (key == "assumedstate") {
            c.assumedState = parseUInt(value, c.assumedState);
        } else if (key == "lognearmisses") {
            c.logNearMisses = parseBool(value);
        } else {
            logger.warn("Unknown [Render] key '" + key + "'.");
        }
    }
}

std::wstring widenUtf8(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

float parseFloat(const std::string& value, float fallback)
{
    try {
        return std::stof(value);
    } catch (...) {
        return fallback;
    }
}

void applyVideoKey(const std::string& key, const std::string& value, Logger& logger)
{
    VideoConfig& v = g_videoConfig;
    if (key == "enabled") {
        g_videoEnabled = parseBool(value);
    } else if (key == "source") {
        v.source = value;
    } else if (key == "playlist") {
        const auto bar = value.find('|');
        if (bar == std::string::npos || bar == 0 || bar + 1 >= value.size()) {
            logger.warn("[Video] playlist needs Name|Source: '" + value + "'");
        } else {
            v.playlists.push_back({ trimCopy(value.substr(0, bar)), trimCopy(value.substr(bar + 1)) });
        }
    } else if (key == "volume_step") {
        v.volumeStep = std::clamp(parseFloat(value, 0.1f), 0.01f, 0.5f);
    } else if (key == "live_buffer_seconds") {
        v.liveBufferSeconds = std::clamp(parseFloat(value, 5.0f), 0.0f, 30.0f);
    } else if (key == "seek_seconds") {
        v.seekSeconds = std::clamp(parseFloat(value, 30.0f), 5.0f, 600.0f);
    } else if (key == "root_path") {
        v.rootDir = widenUtf8(value);
    } else if (key == "ytdlp_path") {
        v.ytdlp = widenUtf8(value);
    } else if (key == "deno_path") {
        v.deno = widenUtf8(value);
    } else if (key == "ffmpeg_path") {
        v.ffmpeg = widenUtf8(value);
    } else if (key == "max_height") {
        v.maxHeight = static_cast<int>(parseUInt(value, 720));
    } else if (key == "volume") {
        v.volume = std::clamp(parseFloat(value, 0.8f), 0.0f, 1.0f);
    } else if (key == "full_volume_meters") {
        v.fullVolumeMeters = std::max(0.1f, parseFloat(value, 2.0f));
    } else if (key == "silent_meters") {
        v.silentMeters = std::max(1.0f, parseFloat(value, 25.0f));
    } else {
        logger.warn("Unknown [Video] key '" + key + "'.");
    }
}

void applyIniConfig(Logger& logger)
{
    const std::filesystem::path iniPath =
        std::filesystem::path("Data") / "SFSE" / "Plugins" / "ScreenSFSE.ini";

    std::ifstream stream(iniPath);
    if (!stream.is_open()) {
        logger.info("Config not found at " + iniPath.string() + ", using default log level.");
        return;
    }

    std::string line;
    std::string section;
    while (std::getline(stream, line)) {
        std::string normalized = trimCopy(line);
        if (normalized.empty()) {
            continue;
        }

        if (normalized[0] == ';' || normalized[0] == '#') {
            continue;
        }
        if (normalized.front() == '[' && normalized.back() == ']') {
            section = toLowerAscii(trimCopy(normalized.substr(1, normalized.size() - 2)));
            continue;
        }

        const std::size_t eq = normalized.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        std::string key = toLowerAscii(trimCopy(normalized.substr(0, eq)));
        std::string value = trimCopy(normalized.substr(eq + 1));
        if ((key == "loglevel" || key == "debuglevel") && !value.empty()) {
            if (!logger.setLevelFromString(value)) {
                logger.warn("Invalid log level '" + value + "' in " + iniPath.string() + '.');
            }
        } else if (section == "surface" || section == "render") {
            applySurfaceKey(section, key, value, logger);
        } else if (section == "video") {
            applyVideoKey(key, value, logger);
        }
    }
}
}

extern "C"
{
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
    SFSEPluginVersionData::kVersion,
    1,
    "ScreenSFSE",
    "comespm",
    SFSEPluginVersionData::kAddressIndependence_AddressLibraryV2,
    SFSEPluginVersionData::kStructureIndependence_NoStructs,
    { RUNTIME_VERSION_1_15_222, 0 },
    0,
    0,
    0
};
}

extern "C" __declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
    try {
        if (!g_logger.initialize()) {
            return false;
        }

        applyIniConfig(g_logger);
        g_logger.info("ScreenSFSE load start.");

        std::vector<ScreenController::ChannelInfo> channels;
        for (const auto& c : kScreenChannels) {
            g_surfaceConfig.channels.push_back({ c.id, c.width, c.height });
            channels.push_back({ c.id, c.key, c.width, c.height, c.id % 10 });
        }
        if (!installVideoSurface(g_logger, g_surfaceConfig)) {
            g_logger.warn("Video surface hooks not (fully) installed; see the lines above.");
        } else if (g_surfaceConfig.enabled && g_videoEnabled) {
            g_screenController = std::make_unique<ScreenController>(g_logger, g_videoConfig, channels);
            PapyrusBridge::setScreenController(g_screenController.get());
        }

        g_screenManager = std::make_unique<ScreenManager>(g_logger);
        g_papyrusBridge = std::make_unique<PapyrusBridge>(g_logger, *g_screenManager);
        if (!g_papyrusBridge->initialize(sfse)) {
            g_logger.error("Papyrus bridge failed to initialize.");
            return false;
        }

        g_logger.info("ScreenSFSE loaded.");
        return true;
    } catch (const std::exception& ex) {
        g_logger.error(std::string("Unhandled exception in SFSEPlugin_Load: ") + ex.what());
        return false;
    } catch (...) {
        g_logger.error("Unhandled unknown exception in SFSEPlugin_Load.");
        return false;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
