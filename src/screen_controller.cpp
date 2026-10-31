#include "screen_controller.h"

#include "live_player.h"
#include "logger.h"
#include "media_player.h"
#include "osd.h"
#include "subtitles.h"
#include "test_pattern.h"
#include "video_fetcher.h"
#include "video_surface.h"

#ifndef SCREENSFSE_HARNESS
#include "RE/P/PlayerCharacter.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <windows.h>
#include <shlobj.h>

namespace
{
// World positions are in metres (RadioSFSE fades over 45 game units = metres). Not the NIF/.mesh scale (0.0142875):
// using that made 20 m read as 0.3 m, so the sound never faded.
constexpr float kMetersPerUnit = 1.0f;
constexpr float kPi = 3.14159265f;

std::string trimPlaylistText(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string lowerPlaylistText(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Parse into temporary values. No running config or playlist is touched on any error.
void readPlaylistDefinitions(const std::filesystem::path& path,
                             std::vector<VideoConfig::PlaylistEntry>& entries, std::string& source)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read ScreenSFSE.ini");
    std::string text(1024 * 1024 + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    if (in.bad() || text.size() > 1024 * 1024) throw std::runtime_error("INI unreadable or larger than 1 MiB");
    if (text.find('\0') != std::string::npos ||
        (!text.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) == 0)) {
        throw std::runtime_error("INI must be UTF-8 text");
    }
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    std::istringstream lines(text);
    std::string line, section;
    bool videoSection = false;
    std::size_t number = 0;
    entries.clear();
    source.clear();
    while (std::getline(lines, line)) {
        ++number;
        line = trimPlaylistText(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        auto bad = [&](const char* why) { throw std::runtime_error("INI line " + std::to_string(number) + ": " + why); };
        if (line.front() == '[') {
            if (line.back() != ']' || line.find(']') != line.size() - 1) bad("invalid section header");
            section = lowerPlaylistText(trimPlaylistText(line.substr(1, line.size() - 2)));
            if (section.empty()) bad("empty section header");
            videoSection |= section == "video";
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos || trimPlaylistText(line.substr(0, eq)).empty() || section.empty()) bad("expected key=value inside a section");
        if (section != "video") continue;
        const auto key = lowerPlaylistText(trimPlaylistText(line.substr(0, eq)));
        const auto value = trimPlaylistText(line.substr(eq + 1));
        static const std::set<std::string> knownKeys = { "enabled", "source", "playlist", "volume_step", "live_buffer_seconds",
            "seek_seconds", "root_path", "ytdlp_path", "deno_path", "ffmpeg_path", "max_height", "volume",
            "full_volume_meters", "silent_meters", "subtitle_languages", "subtitle_size", "mute_game_music" };
        if (!knownKeys.contains(key)) bad("unknown [Video] key");
        if (key == "source") {
            if (value.size() > 4096) bad("source is too long");
            source = value;
        }
        if (key != "playlist") continue;
        const auto bar = value.find('|');
        if (bar == std::string::npos) bad("playlist needs Name|Source");
        auto name = trimPlaylistText(value.substr(0, bar));
        auto location = trimPlaylistText(value.substr(bar + 1));
        VideoConfig::PlaylistEntry entry;
        splitPlaylistOptions(location, entry);
        if (name.empty() || location.empty()) bad("playlist name and source must not be empty");
        if (name.size() > 128 || location.size() > 4096 || entries.size() >= 128) bad("playlist limits exceeded");
        for (const auto& other : entries) {
            if (lowerPlaylistText(other.name) == lowerPlaylistText(name)) bad("duplicate playlist name");
        }
        entry.name = std::move(name);
        entry.source = std::move(location);
        entries.push_back(std::move(entry));
    }
    if (!videoSection) throw std::runtime_error("INI has no [Video] section");
}

}

void splitPlaylistOptions(std::string& source, VideoConfig::PlaylistEntry& options)
{
    for (;;) {
        const auto bar = source.rfind('|');
        if (bar == std::string::npos) {
            return;
        }
        const auto option = trimPlaylistText(source.substr(bar + 1));
        const auto lower = lowerPlaylistText(option);
        if (lower.starts_with("subs=")) {
            options.subtitles = trimPlaylistText(option.substr(5));
        } else if (lower.starts_with("items=")) {
            options.items = trimPlaylistText(option.substr(6));
        } else if (lower == "stream") {
            options.streamOnly = true;
        } else {
            return;
        }
        source = trimPlaylistText(source.substr(0, bar));
    }
}

void splitPlaylistOptions(std::string& source, std::string& subtitles)
{
    const auto bar = source.rfind('|');
    if (bar == std::string::npos) {
        return;
    }
    const auto option = trimPlaylistText(source.substr(bar + 1));
    if (lowerPlaylistText(option.substr(0, 5)) != "subs=") {
        return;
    }
    subtitles = trimPlaylistText(option.substr(5));
    source = trimPlaylistText(source.substr(0, bar));
}

std::vector<std::string> splitLanguages(const std::string& list)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= list.size()) {
        auto end = list.find(',', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        const auto item = trimPlaylistText(list.substr(start, end - start));
        const bool known = std::any_of(out.begin(), out.end(), [&](const std::string& l) { return lowerPlaylistText(l) == lowerPlaylistText(item); });
        if (!item.empty() && item.size() <= 16 && !known) {
            out.push_back(item);
        }
        start = end + 1;
    }
    return out;
}

namespace
{
// The player's position and heading straight from the game's player reference (CommonLibSF layout).
// Plain data and SEH only, so a wrong layout or pointer gives `false`, not a crash. Only trusted after
// registerScreen() has compared it with Papyrus' GetPosition values.
bool readPlayer(float& x, float& y, float& z, float& heading)
{
#ifdef SCREENSFSE_HARNESS   // tests/controller_harness.cpp: no game, no Address Library
    (void)x, (void)y, (void)z, (void)heading;
    return false;
#else
    __try {
        RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }
        const RE::TESObjectREFR* ref = player;
        x = ref->data.location.x;
        y = ref->data.location.y;
        z = ref->data.location.z;
        heading = ref->data.angle.z;
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(heading);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

std::string narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>((std::max)(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>((std::max)(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// %VAR% expansion (RadioSFSE.ini writes deno_path under %USERPROFILE%)
std::filesystem::path expand(const std::filesystem::path& path)
{
    const std::wstring in = path.wstring();
    if (in.find(L'%') == std::wstring::npos) {
        return path;
    }
    const DWORD size = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
    std::wstring out(size, L' ');
    const DWORD written = ExpandEnvironmentStringsW(in.c_str(), out.data(), size);
    if (written == 0 || written > size) {
        return path;
    }
    out.resize(written - 1);
    return out;
}

std::filesystem::path documentsDir()
{
    PWSTR raw = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw)) && raw) {
        out = raw;
    }
    CoTaskMemFree(raw);
    return out;
}

std::filesystem::path onPath(const wchar_t* exe)
{
    wchar_t buffer[MAX_PATH] = {};
    if (SearchPathW(nullptr, exe, nullptr, MAX_PATH, buffer, nullptr) > 0) {
        return buffer;
    }
    return {};
}

// `key=value` from RadioSFSE.ini (same tool settings as the radio's YouTube media type)
std::filesystem::path radioIniValue(const char* key)
{
    std::ifstream in(std::filesystem::path("Data") / "SFSE" / "Plugins" / "RadioSFSE.ini");
    std::string line;
    const std::string want = key;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string k = line.substr(0, eq);
        k.erase(std::remove_if(k.begin(), k.end(), ::isspace), k.end());
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        if (k == want) {
            std::string v = line.substr(eq + 1);
            v.erase(0, v.find_first_not_of(" \t\""));
            v.erase(v.find_last_not_of(" \t\r\"") + 1);
            return std::filesystem::path(widen(v));
        }
    }
    return {};
}

std::filesystem::path findTool(const std::filesystem::path& configured, const char* radioKey, const std::filesystem::path& root,
                               const wchar_t* exe)
{
    std::error_code ec;
    for (const auto& candidate : { expand(configured), expand(radioIniValue(radioKey)), root / L"Tools" / exe, onPath(exe) }) {
        if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

bool isVideoFile(const std::filesystem::path& p)
{
    const std::wstring ext = p.extension().wstring();
    for (const wchar_t* e : { L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".mkv" }) {
        if (_wcsicmp(ext.c_str(), e) == 0) {
            return true;
        }
    }
    return false;
}

std::string clockText(double seconds)
{
    const int s = static_cast<int>(std::max(0.0, seconds));
    char buf[32];
    if (s >= 3600) {
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    }
    return buf;
}

std::string shorten(const std::string& text, std::size_t max)
{
    if (text.size() <= max) {
        return text;
    }
    std::size_t cut = max - 3;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {   // keep UTF-8 whole
        --cut;
    }
    return text.substr(0, cut) + "...";
}

std::string fileKey(const std::string& name)
{
    std::string out;
    for (char c : name) {
        out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return out;
}

std::string listCacheKey(const std::string& name, const std::string& source)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : source) hash = (hash ^ c) * 1099511628211ull;
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "_%016llx", static_cast<unsigned long long>(hash));
    return fileKey(name) + suffix; // replacing a URL under the same name must not reuse its old listing
}
}

namespace
{
// Remote actions (remoteAction): the terminal menu hides the game while it is open, so their notices stay up longer.
thread_local int g_noticeScale = 1;

// TV-style notice over a channel's picture (osd.h): 3 s, 4 s with a second line (x2 from the remote).
void tvNotice(int channel, Osd::Icon icon, const std::string& label, const std::string& detail = {}, float level = -1.0f)
{
    Osd osd;
    osd.icon = icon;
    osd.label = osdText(label, 24);
    osd.detail = osdText(detail, 40);
    osd.level = level;
    osd.until = std::chrono::steady_clock::now() + std::chrono::seconds((detail.empty() ? 3 : 4) * g_noticeScale);
    showOsd(channel, osd);
}
}

ScreenController::ScreenController(Logger& logger, const VideoConfig& config, const std::vector<ChannelInfo>& channels) :
    logger_(logger), config_(config)
{
    // Default Data\VideoScreen in the game folder, as the radio's root_path=Data\Radio (players did not find their
    // media under Documents). Not Data\Video: that is the game's own Bink video folder. A relative root_path is
    // relative to the game folder (the game's working directory).
    if (config_.rootDir.empty()) {
        config_.rootDir = std::filesystem::path(L"Data") / L"VideoScreen";
    }
    config_.rootDir = expand(config_.rootDir);
    if (config_.rootDir.is_relative()) {
        config_.rootDir = std::filesystem::absolute(config_.rootDir);
    }
    std::error_code ec;
    std::filesystem::create_directories(config_.rootDir / L"Videos", ec);
    std::filesystem::create_directories(config_.rootDir / L"Cache", ec);
    VideoFetcher::Settings fs;
    fs.ytdlp = findTool(config_.ytdlp, "ytdlp_path", config_.rootDir, L"yt-dlp.exe");
    fs.ffmpeg = findTool(config_.ffmpeg, "ffmpeg_path", config_.rootDir, L"ffmpeg.exe");
    fs.deno = findTool(config_.deno, "deno_path", config_.rootDir, L"deno.exe");
    if (fs.deno.empty()) {
        const auto home = expand(L"%USERPROFILE%") / L".deno" / L"bin" / L"deno.exe";
        if (std::filesystem::exists(home, ec)) {
            fs.deno = home;
        }
    }
    fs.cacheDir = config_.rootDir / L"Cache";
    fs.maxHeight = config_.maxHeight;
    fetcher_ = std::make_unique<VideoFetcher>(logger_);
    fetcher_->configure(fs);

    playlists_ = buildPlaylists(config_.playlists, config_.source);
    std::string names;
    for (const auto& pl : playlists_) {
        names += (names.empty() ? "" : ", ") + pl.name + (pl.online ? " (online)" : " (folder)");
    }
    logger_.info("Video root " + narrow(config_.rootDir.wstring()) + "; yt-dlp " +
                 (fs.ytdlp.empty() ? std::string("NOT FOUND") : narrow(fs.ytdlp.wstring())) + "; ffmpeg " +
                 (fs.ffmpeg.empty() ? std::string("not found") : narrow(fs.ffmpeg.wstring())) + "; deno " +
                 (fs.deno.empty() ? std::string("not found") : narrow(fs.deno.wstring())) + ". Playlists: " + names);
    std::string languages;
    for (const auto& l : config_.subtitleLanguages) {
        languages += (languages.empty() ? "" : ", ") + l;
    }
    logger_.info("Subtitles: " + (languages.empty() ? std::string("local untagged .srt files only (subtitle_languages empty)")
                                                    : "languages " + languages) + ", size " + std::to_string(config_.subtitleSize).substr(0, 4) + ".");

    for (const auto& info : channels) {
        auto c = std::make_unique<Channel>();
        c->info = info;
        c->volume = config_.volume;
        // wall1 keeps the single-screen session file of v3
        c->sessionFile = config_.rootDir / widen(info.key == "wall1" ? std::string("ScreenSFSE.session.txt")
                                                                       : "ScreenSFSE.session." + info.key + ".txt");
        loadSession(*c);
        // stopped = switched off: black; the colour bars only while loading/buffering
        c->idleCard = std::make_shared<BlackSource>(info.width, info.height);
        setFrameSource(info.id, c->idleCard);
        if (c->sessionPlaying || !c->pendingTrackKey.empty()) {
            logger_.info(tag(*c) + "session: playlist " + playlists_[c->playlist].name + ", track '" + c->pendingTrackKey +
                         "' at " + clockText(c->resumeAt) + ", volume " + std::to_string(static_cast<int>(c->volume * 100 + 0.5f)) +
                         "%" + (c->sessionPlaying ? ", was playing" : ""));
        }
        channels_.push_back(std::move(c));
    }
    worker_ = std::thread([this] { spatialLoop(); });
}

ScreenController::~ScreenController()
{
    {
        std::lock_guard lock(mutex_);
        for (auto& c : channels_) {
            if (c->state == State::Playing || c->state == State::Paused) {
                c->resumeAt = positionLocked(*c);
                saveSessionLocked(*c);
            }
            ++c->generation;
        }
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    fetcher_->shutdown();
    for (auto& c : channels_) {
        setFrameSource(c->info.id, nullptr);
        c->captions.reset();
        c->player.reset();
    }
}

std::string ScreenController::tag(const Channel& c) const
{
    return "[" + c.info.key + "] ";
}

ScreenController::Channel* ScreenController::channelById(int id)
{
    for (auto& c : channels_) {
        if (c->info.id == id) {
            return c.get();
        }
    }
    return nullptr;
}

ScreenController::Channel* ScreenController::channelFor(std::uint32_t formId)
{
    const auto it = screens_.find(formId);
    Channel* c = it != screens_.end() ? channelById(it->second.channel) : nullptr;
    return c ? c : (channels_.empty() ? nullptr : channels_.front().get());
}

// ---- session -----------------------------------------------------------------------------------------------------

void ScreenController::loadSession(Channel& c)
{
    std::ifstream in(c.sessionFile);
    readSession(c, in);
}

// key=value lines of one channel (a session file, or one [key] section of a save snapshot); stops at a section header.
void ScreenController::readSession(Channel& c, std::istream& in)
{
    std::string line;
    while (in.peek() != '[' && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        try {
            if (k == "playlist") {
                for (std::size_t i = 0; i < playlists_.size(); ++i) {
                    if (playlists_[i].name == v) {
                        c.playlist = i;
                    }
                }
            } else if (k == "track") {
                c.pendingTrackKey = v;
            } else if (k == "position") {
                c.resumeAt = std::max(0.0, std::stod(v));
            } else if (k == "volume") {
                c.volume = std::clamp(std::stof(v), 0.0f, 1.0f);
            } else if (k == "playing") {
                c.sessionPlaying = v == "1";
            } else if (k == "screen") {
                c.sessionScreen = static_cast<std::uint32_t>(std::stoul(v, nullptr, 16));
            } else if (k == "subtitles") {
                c.subtitleChoice = v;
            } else if (k == "where") {
                float p[3]{};
                if (std::sscanf(v.c_str(), "%f,%f,%f", &p[0], &p[1], &p[2]) == 3) {
                    std::copy(p, p + 3, c.sessionWhere);
                }
            } else if (k == "area") {
                unsigned world = 0, cell = 0;
                if (std::sscanf(v.c_str(), "%x,%x", &world, &cell) == 2) c.sessionArea = { world, cell };
            }
        } catch (...) {
        }
    }
}

// Caller holds mutex_. Back to a channel with nothing chosen yet (before reading a save's state into it).
void ScreenController::resetSessionLocked(Channel& c)
{
    c.playlist = 0;
    c.track = 0;
    c.pendingTrackKey.clear();
    c.resumeAt = 0;
    c.volume = config_.volume;
    c.sessionPlaying = false;
    c.sessionScreen = 0;
    std::fill(c.sessionWhere, c.sessionWhere + 3, 0.0f);
    c.sessionArea = {};
    c.subtitleChoice.clear();
    c.active = 0;
    c.resumeOnLoad = false;
    c.pendingMessage.clear();
}

std::string ScreenController::sessionTextLocked(const Channel& c, double position, bool playing) const
{
    const Track* t = currentTrackLocked(c);
    const std::string key = !c.pendingTrackKey.empty() ? c.pendingTrackKey : t ? t->key : std::string();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", position);
    std::string text = "playlist=" + (playlists_.empty() ? std::string() : playlists_[c.playlist].name) + "\n" + "track=" + key +
                       "\n" + "position=" + buf + "\n";
    std::snprintf(buf, sizeof(buf), "%.2f", c.volume);
    text += std::string("volume=") + buf + "\n" + "playing=" + (playing ? "1" : "0") + "\n";
    const auto screenId = c.active ? c.active : c.sessionScreen;
    std::snprintf(buf, sizeof(buf), "%08X", screenId);
    text += std::string("screen=") + buf + "\n";
    if (const auto it = screens_.find(screenId); it != screens_.end()) {
        std::snprintf(buf, sizeof(buf), "%.1f,%.1f,%.1f", it->second.x, it->second.y, it->second.z);
        text += std::string("where=") + buf + "\n";
        std::snprintf(buf, sizeof(buf), "%08X,%08X", it->second.area.world, it->second.area.cell);
        text += std::string("area=") + buf + "\n";
    } else if (screenId) {
        std::snprintf(buf, sizeof(buf), "%.1f,%.1f,%.1f", c.sessionWhere[0], c.sessionWhere[1], c.sessionWhere[2]);
        text += std::string("where=") + buf + "\n";
        std::snprintf(buf, sizeof(buf), "%08X,%08X", c.sessionArea.world, c.sessionArea.cell);
        text += std::string("area=") + buf + "\n";
    }
    if (!c.subtitleChoice.empty()) {
        text += "subtitles=" + c.subtitleChoice + "\n";
    }
    return text;
}

void ScreenController::saveSessionLocked(Channel& c)
{
    const bool playing = c.state == State::Playing || c.state == State::Fetching || c.resumeOnLoad || c.sessionPlaying;
    const auto temp = c.sessionFile.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << sessionTextLocked(c, c.resumeAt, playing);
    }
    MoveFileExW(temp.c_str(), c.sessionFile.c_str(), MOVEFILE_REPLACE_EXISTING);
    c.lastSessionSave = std::chrono::steady_clock::now();
}

// ---- per-save snapshots ------------------------------------------------------------------------------------------

namespace
{
constexpr std::size_t kMaxSnapshots = 500;   // ~1 KB each; the oldest go first (saves deleted in game are never known)

std::filesystem::path snapshotFile(const std::filesystem::path& root, std::uint32_t token)
{
    return root / L"Saves" / (std::to_wstring(token) + L".txt");
}
}

void ScreenController::saveSnapshot(std::uint32_t token)
{
    std::lock_guard lock(mutex_);
    reconcileAudienceLocked();
    std::string text;
    int playing = 0;
    for (const auto& cp : channels_) {
        const Channel& c = *cp;
        // a channel paused only because its screen unloaded was playing for the player
        const bool on = c.state == State::Playing || c.state == State::Fetching || c.resumeOnLoad || c.sessionPlaying;
        const double at = c.state == State::Playing || c.state == State::Paused ? positionLocked(c) : c.resumeAt;
        playing += on ? 1 : 0;
        text += "[" + c.info.key + "]\n" + sessionTextLocked(c, at, on);
    }
    const auto dir = config_.rootDir / L"Saves";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = snapshotFile(config_.rootDir, token);
    const auto temp = file.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << text;
        if (!out) {
            logger_.warn("Save snapshot " + std::to_string(token) + " could not be written.");
            return;
        }
    }
    MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    logger_.info("Save snapshot " + std::to_string(token) + " written: " + std::to_string(playing) + " channel(s) playing.");
    trimSnapshots(dir);
}

void ScreenController::trimSnapshots(const std::filesystem::path& dir) const
{
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == L".txt") {
            files.emplace_back(it->last_write_time(ec), it->path());
        }
    }
    if (files.size() <= kMaxSnapshots) {
        return;
    }
    std::sort(files.begin(), files.end());
    for (std::size_t i = 0; i + kMaxSnapshots < files.size(); ++i) {
        std::filesystem::remove(files[i].second, ec);
    }
}

void ScreenController::beginLoad()
{
    std::lock_guard lock(mutex_);
    loading_ = true;
    playerArea_ = {};
    for (auto& cp : channels_) {
        Channel& c = *cp;
        if (c.state != State::Idle) {
            c.resumeAt = positionLocked(c);
            saveSessionLocked(c);
            stopLocked(c, "a save is loading");
        }
    }
    screens_.clear();   // form IDs of the previous game; the loaded save's screens register again as they load
}

void ScreenController::finishLoad(std::uint32_t token, const ScreenProbe& probe)
{
    std::lock_guard lock(mutex_);
    loading_ = false;
    std::ifstream in;
    if (token != 0) {
        in.open(snapshotFile(config_.rootDir, token));
    }
    const bool snapshot = in.is_open();
    std::unordered_map<std::string, std::string> sections;
    if (snapshot) {
        std::string line, key;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
                key = line.substr(1, line.size() - 2);
            } else if (!key.empty()) {
                sections[key] += line + "\n";
            }
        }
    }
    logger_.info(snapshot ? "Save loaded: applying snapshot " + std::to_string(token) + "."
                          : token != 0 ? "Save loaded: no snapshot " + std::to_string(token) + "; using the session files."
                                       : "Save loaded: made before per-save snapshots; using the session files.");
    int resumed = 0, waiting = 0;
    for (auto& cp : channels_) {
        Channel& c = *cp;
        if (c.state != State::Idle) {
            c.resumeAt = positionLocked(c);
            saveSessionLocked(c);
            stopLocked(c, "a save was loaded");
        }
        resetSessionLocked(c);
        if (snapshot) {
            const auto it = sections.find(c.info.key);
            if (it != sections.end()) {
                std::istringstream lines(it->second);
                readSession(c, lines);
            }
        } else {
            loadSession(c);
        }
        if (!c.sessionPlaying) {
            continue;
        }
        // its screen may have loaded (and registered) before the load finished
        auto screen = std::find_if(screens_.begin(), screens_.end(), [&](const auto& s) {
            return s.second.loaded && s.second.channel == c.info.id && (s.first == c.sessionScreen || c.sessionScreen == 0);
        });
        float x = c.sessionWhere[0], y = c.sessionWhere[1], z = c.sessionWhere[2];
        auto area = c.sessionArea;
        if (screen == screens_.end() && c.sessionScreen != 0 && probe && probe(c.sessionScreen, c.info.id, x, y, z, area)) {
            // already in the world: its OnLoad did not run for this load
            float rx = 0, ry = 0, rz = 0, heading = 0;
            if (readPlayer(rx, ry, rz, heading)) {
                lastPlayer_[0] = rx;
                lastPlayer_[1] = ry;
                lastPlayer_[2] = rz;
                if (!playerPositionChecked_) {
                    // no screen has registered yet, so the direct read is not verified against Papyrus: without it the
                    // sound would be placed at (0, 0, 0) and stay silent. The next registration checks it again.
                    playerPositionVerified_ = true;
                    logger_.info("Player position read directly for the resumed screen (checked again when a screen registers).");
                }
            }
            if (x == 0 && y == 0 && z == 0) {
                x = lastPlayer_[0];   // no saved position (older snapshot): the player is near it
                y = lastPlayer_[1];
                z = lastPlayer_[2];
            }
            Screen& s = screens_[c.sessionScreen];
            s.channel = c.info.id;
            s.x = x;
            s.y = y;
            s.z = z;
            s.loaded = true;
            s.area = area;
            screen = screens_.find(c.sessionScreen);
            logger_.info(tag(c) + "Its screen is already loaded in the world (no OnLoad for this load).");
        }
        if (screen == screens_.end()) {
            ++waiting;
            continue;
        }
        c.sessionPlaying = false;
        c.active = screen->first;
        logger_.info(tag(c) + "Resuming the save's video at " + clockText(c.resumeAt) + ".");
        playCurrentLocked(c, c.resumeAt);
        tvNotice(c.info.id, Osd::Icon::Play, "PLAY", trackLabelLocked(c));
        ++resumed;
    }
    if (resumed + waiting > 0) {
        logger_.info("Save loaded: " + std::to_string(resumed) + " channel(s) resumed, " + std::to_string(waiting) +
                     " waiting for their screen to load.");
    }
}

// ---- playlists ---------------------------------------------------------------------------------------------------

std::vector<ScreenController::Playlist> ScreenController::buildPlaylists(
    const std::vector<VideoConfig::PlaylistEntry>& configured, const std::string& source) const
{
    auto entries = configured;
    if (entries.empty() && !source.empty()) entries.push_back({ "Video", source });
    std::vector<Playlist> result;
    std::set<std::string> names;
    std::vector<std::wstring> localPaths;
    auto localPath = [&](const std::string& value) {
        auto path = std::filesystem::path(widen(value));
        if (path.is_relative()) path = config_.rootDir / path;
        return path.lexically_normal().wstring();
    };
    auto containsLocal = [&](const std::filesystem::path& path) {
        const auto value = path.lexically_normal().wstring();
        return std::any_of(localPaths.begin(), localPaths.end(), [&](const auto& p) { return _wcsicmp(p.c_str(), value.c_str()) == 0; });
    };
    auto add = [&](const VideoConfig::PlaylistEntry& entry) {
        if (result.size() >= 512) throw std::runtime_error("too many playlists (maximum 512)");
        if (entry.name.empty() || entry.source.empty() || !names.insert(lowerPlaylistText(entry.name)).second) {
            throw std::runtime_error("empty or duplicate playlist name: " + entry.name);
        }
        Playlist pl;
        pl.name = entry.name;
        pl.source = entry.source;
        pl.subtitles = entry.subtitles;
        pl.online = VideoFetcher::onlineSource(entry.source);
        if (pl.online) {
            pl.items = entry.items;
            pl.streamOnly = entry.streamOnly;
        } else {
            localPaths.push_back(localPath(pl.source));
            pl.source = narrow(localPaths.back()); // stable identity for relative/absolute INI aliases and folder discovery
            listLocal(pl);
        }
        result.push_back(std::move(pl));
    };
    for (const auto& entry : entries) add(entry);
    const auto videos = config_.rootDir / L"Videos";
    if (!containsLocal(videos)) add({ "My Videos", "Videos" });
    std::error_code ec;
    std::vector<std::filesystem::path> folders;
    for (std::filesystem::recursive_directory_iterator it(videos, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            it.disable_recursion_pending(); // never follow junctions/symlinks outside Videos or around a cycle
            continue;
        }
        if (it->is_directory(ec)) {
            if (it.depth() >= 32 || folders.size() >= 512) throw std::runtime_error("local folder limits exceeded");
            folders.push_back(it->path());
        }
    }
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) {
        if (!containsLocal(folder)) {
            add({ "Local: " + narrow(folder.lexically_relative(videos).generic_wstring()), narrow(folder.wstring()) });
        }
    }
    return result;
}

std::string ScreenController::refreshPlaylistsLocked()
{
    try {
        auto entries = config_.playlists;
        auto source = config_.source;
        if (!config_.playlistIni.empty()) readPlaylistDefinitions(config_.playlistIni, entries, source);
        auto replacement = buildPlaylists(entries, source);
        // Keep usable online listings while a new listing is fetched. Match by source, never name alone.
        for (auto& pl : replacement) {
            if (!pl.online) continue;
            for (const auto& old : playlists_) {
                if (old.online && old.source == pl.source && old.items == pl.items) {
                    pl.tracks = old.tracks;
                    pl.listed = old.listed;
                    break;
                }
            }
        }
        struct Selection { std::size_t playlist = 0, track = 0; bool retain = false, restart = false; std::string pending; };
        std::vector<Selection> selections;
        for (const auto& c : channels_) {
            const auto& old = playlists_[c->playlist];
            const auto* track = currentTrackLocked(*c);
            const auto key = track ? track->key : c->pendingTrackKey;
            Selection selection;
            // Prefer an exact match, then allow an INI rename of the same source.
            auto found = std::find_if(replacement.begin(), replacement.end(), [&](const auto& p) { return p.name == old.name && p.source == old.source; });
            if (found == replacement.end()) found = std::find_if(replacement.begin(), replacement.end(), [&](const auto& p) { return p.source == old.source; });
            if (found != replacement.end()) {
                selection.playlist = static_cast<std::size_t>(found - replacement.begin());
                auto t = std::find_if(found->tracks.begin(), found->tracks.end(), [&](const auto& item) { return item.key == key; });
                selection.retain = key.empty() || !found->listed || t != found->tracks.end();
                if (t != found->tracks.end()) selection.track = static_cast<std::size_t>(t - found->tracks.begin());
                if (!found->listed) selection.pending = key;
                // A pending download's per-channel callback remains valid. Only listing callbacks were invalidated.
                selection.restart = selection.retain && c->state == State::Fetching && !old.listed;
            } else {
                const auto videos = narrow((config_.rootDir / L"Videos").lexically_normal().wstring());
                auto local = std::find_if(replacement.begin(), replacement.end(), [&](const auto& p) { return p.source == videos; });
                if (local != replacement.end()) selection.playlist = static_cast<std::size_t>(local - replacement.begin());
            }
            selections.push_back(std::move(selection));
        }
        // All validation and catalog construction succeeded. Commit once, then invalidate old async listings.
        ++playlistGeneration_;
        playlists_.swap(replacement);
        config_.playlists = std::move(entries);
        config_.source = std::move(source);
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            auto& c = *channels_[i];
            auto& s = selections[i];
            if (s.retain) c.resumeAt = positionLocked(c);
            if (!s.retain || s.restart) {
                stopLocked(c, "playlists refreshed");
                if (!s.retain) {
                    c.resumeAt = 0;
                    c.sessionPlaying = false;
                    c.pendingMessage = "Video Screen: selected video or playlist was removed";
                }
            }
            c.playlist = s.playlist;
            c.track = s.track;
            c.pendingTrackKey = std::move(s.pending);
            if (s.restart) playCurrentLocked(c, c.resumeAt);
            saveSessionLocked(c);
        }
        for (auto& pl : playlists_) if (pl.online && pl.listed) refreshListLocked(pl);
        logger_.info("Playlists refreshed: " + std::to_string(playlists_.size()) + " playlists; online listings update in background.");
        return "Playlists refreshed (" + std::to_string(playlists_.size()) + "); online listings update in background";
    } catch (const std::exception& error) {
        logger_.warn(std::string("Playlist refresh failed: ") + error.what());
        return std::string("Playlists not refreshed: ") + error.what();
    }
}

void ScreenController::listLocal(Playlist& pl) const
{
    pl.tracks.clear();
    std::filesystem::path p = widen(pl.source);
    if (p.is_relative()) {
        p = config_.rootDir / p;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) {
        pl.tracks.push_back({ narrow(p.wstring()), narrow(p.stem().wstring()) });
    } else {
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(p, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->is_regular_file(ec) && isVideoFile(it->path())) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            pl.tracks.push_back({ narrow(f.wstring()), narrow(f.stem().wstring()) });
        }
    }
    pl.listed = true;
}

void ScreenController::loadListCache(Playlist& pl) const
{
    const std::string identity = pl.items.empty() ? pl.source : pl.source + "|items=" + pl.items;
    std::ifstream in(config_.rootDir / L"Cache" / widen("list_" + listCacheKey(pl.name, identity) + ".tsv"));
    std::string line;
    std::vector<Track> tracks;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // YouTube: <id> TAB <live_status> TAB <title>; older caches: <id> TAB <title>.
        // Other sites: <key> TAB <status> TAB <title> TAB <url> (keys are never 11 characters long).
        const auto tab = line.find('\t');
        if (tab != 11) {
            const auto tab2 = tab == std::string::npos ? tab : line.find('\t', tab + 1);
            const auto last = line.rfind('\t');
            if (tab != std::string::npos && tab > 0 && tab2 != std::string::npos && last > tab2) {
                tracks.push_back({ line.substr(0, tab), line.substr(tab2 + 1, last - tab2 - 1),
                                   line.substr(tab + 1, tab2 - tab - 1), line.substr(last + 1) });
            }
            continue;
        }
        const auto tab2 = line.find('\t', 12);
        const std::string mid = tab2 == std::string::npos ? std::string() : line.substr(12, tab2 - 12);
        const bool hasStatus = !mid.empty() && mid.size() <= 12 && mid.find(' ') == std::string::npos &&
                               (mid.find('_') != std::string::npos || mid == "NA" || mid == "stream");
        if (hasStatus) {
            tracks.push_back({ line.substr(0, 11), line.substr(tab2 + 1), mid });
        } else {
            tracks.push_back({ line.substr(0, 11), line.substr(12), std::string() });
        }
    }
    if (!tracks.empty()) {
        pl.tracks = std::move(tracks);
        pl.listed = true;
    }
}

void ScreenController::saveListCache(const Playlist& pl) const
{
    const std::string identity = pl.items.empty() ? pl.source : pl.source + "|items=" + pl.items;
    std::ofstream out(config_.rootDir / L"Cache" / widen("list_" + listCacheKey(pl.name, identity) + ".tsv"), std::ios::trunc);
    for (const auto& t : pl.tracks) {
        out << t.key << '\t' << (t.status.empty() ? std::string("NA") : t.status) << '\t' << t.title;
        if (!t.url.empty()) {
            out << '\t' << t.url;
        }
        out << '\n';
    }
}

// Background refresh of a listed online playlist, once per game session (new uploads). Channels keep pointing at
// the video they were on.
void ScreenController::refreshListLocked(Playlist& pl)
{
    if (pl.refreshed || pl.listing) {
        return;
    }
    pl.refreshed = true;
    pl.listing = true;
    const std::string name = pl.name;
    const auto epoch = playlistGeneration_;
    fetcher_->requestList(pl.source, [this, name, epoch](bool ok, std::vector<VideoFetcher::Track> tracks, const std::string&) {
        std::lock_guard lock(mutex_);
        if (epoch != playlistGeneration_) return;
        for (std::size_t i = 0; i < playlists_.size(); ++i) {
            Playlist& p = playlists_[i];
            if (p.name != name) {
                continue;
            }
            p.listing = false;
            if (!ok) {
                continue;
            }
            std::vector<std::pair<Channel*, std::string>> keep;
            std::vector<Channel*> nextBroadcast;   // waiting on a live video that is gone: play the new one
            for (auto& c : channels_) {
                if (c->playlist == i && currentTrackLocked(*c)) {
                    const Track& t = *currentTrackLocked(*c);
                    const bool live = t.status == "is_live" || t.status == "is_upcoming";
                    if (live && (c->deadLiveKey == t.key || c->state == State::Fetching) &&
                        std::none_of(tracks.begin(), tracks.end(), [&](const auto& n) { return n.id == t.key; })) {
                        nextBroadcast.push_back(c.get());
                    } else {
                        keep.emplace_back(c.get(), t.key);
                    }
                }
                if (c->playlist == i) {
                    c->deadLiveKey.clear();
                }
            }
            std::set<std::string> streamed;   // learned when played: no H.264 download (yt-dlp lists them as NA)
            for (const auto& t : p.tracks) {
                if (t.status == "stream") streamed.insert(t.key);
            }
            p.tracks.clear();
            for (auto& t : tracks) {
                const bool live = t.status == "is_live" || t.status == "is_upcoming";
                p.tracks.push_back({ t.id, t.title, !live && streamed.contains(t.id) ? std::string("stream") : t.status, t.url });
            }
            saveListCache(p);
            for (auto& [c, key] : keep) {
                bool found = false;
                for (std::size_t k = 0; k < p.tracks.size(); ++k) {
                    if (p.tracks[k].key == key) {
                        c->track = k;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    stopLocked(*c, "video removed from playlist");
                    c->track = 0;
                    c->resumeAt = 0;
                    c->pendingTrackKey.clear();
                    c->pendingMessage = "Video Screen: selected video was removed from the playlist";
                }
                c->resumeAt = positionLocked(*c);
                saveSessionLocked(*c);
            }
            for (Channel* c : nextBroadcast) {
                stopLocked(*c, "broadcast ended");
                c->track = 0;
                c->resumeAt = 0;
                if (!p.tracks.empty()) {
                    logger_.info(tag(*c) + "Next broadcast: " + p.tracks[0].key + ".");
                    playCurrentLocked(*c, 0);
                    tvNotice(c->info.id, Osd::Icon::Play, "LIVE", trackLabelLocked(*c));
                }
                saveSessionLocked(*c);
            }
        }
    }, pl.items);
}

// True when the playlist's tracks are known. Otherwise starts listing it (online) and plays channel c from `at` when done.
bool ScreenController::ensureListedLocked(Channel& c, Playlist& pl, double at)
{
    if (!pl.online) {
        if (!pl.listed) {
            listLocal(pl);
        }
        return true;
    }
    if (!pl.listed) {
        loadListCache(pl);   // last session's listing: start at once, refreshed in the background
    }
    if (pl.listed) {
        refreshListLocked(pl);
        return true;
    }
    if (!pl.listing) {
        pl.listing = true;
        pl.refreshed = true;
        const std::string name = pl.name;
        const auto epoch = playlistGeneration_;
        fetcher_->requestList(pl.source, [this, name, epoch](bool ok, std::vector<VideoFetcher::Track> tracks, const std::string& error) {
            std::lock_guard lock(mutex_);
            if (epoch != playlistGeneration_) return;
            for (auto& p : playlists_) {
                if (p.name != name) {
                    continue;
                }
                p.listing = false;
                if (ok) {
                    p.tracks.clear();
                    for (auto& t : tracks) {
                        p.tracks.push_back({ t.id, t.title, t.status, t.url });
                    }
                    p.listed = true;
                    saveListCache(p);
                }
            }
            // every channel waiting on this playlist goes on (or reports the failure)
            for (auto& ch : channels_) {
                if (ch->state != State::Fetching || playlists_[ch->playlist].name != name || ch->player) {
                    continue;
                }
                if (!ok) {
                    ch->state = State::Idle;
                    showErrorLocked(*ch, "NO SIGNAL", name);
                    ch->pendingMessage = "Video Screen: cannot list the playlist - " + shorten(error, 100);
                    continue;
                }
                playCurrentLocked(*ch, ch->resumeAt);
            }
        }, pl.items);
    }
    c.resumeAt = at;
    c.state = State::Fetching;
    showLoadingLocked(c);
    return false;
}

const ScreenController::Track* ScreenController::currentTrackLocked(const Channel& c) const
{
    if (playlists_.empty()) {
        return nullptr;
    }
    const Playlist& pl = playlists_[c.playlist];
    return c.track < pl.tracks.size() ? &pl.tracks[c.track] : nullptr;
}

std::string ScreenController::trackLabelLocked(const Channel& c) const
{
    const Track* t = currentTrackLocked(c);
    return t ? shorten(t->title, 60) : std::string("(no video)");
}

// ---- playback ----------------------------------------------------------------------------------------------------

void ScreenController::updatePlayerAreaLocked()
{
#ifndef SCREENSFSE_HARNESS
    const auto context = ScreenLocation::player();
    if (context != playerArea_) {
        char line[128];
        std::snprintf(line, sizeof(line), "Playback area: world %08X, cell %08X, landed-ship view %08X.",
                      context.area.world, context.area.cell, context.visibleWorld);
        logger_.info(line);
        playerArea_ = context;
    }
#endif
}

void ScreenController::setPlayerArea(ScreenLocation::Context context)
{
    std::lock_guard lock(mutex_);
    areaTracking_ = true;
    playerArea_ = context;
    reconcileAudienceLocked();
}

bool ScreenController::screenEligibleLocked(const Screen& s) const
{
    if (!s.loaded) return false;
    if (!areaTracking_) return true;
    if (!playerArea_.area.cell || !s.area.cell) return false;
    if (s.area.world) return s.area.world == playerArea_.area.world || s.area.world == playerArea_.visibleWorld;
    return s.area.cell == playerArea_.area.cell;
}

bool ScreenController::selectAudienceLocked(Channel& c)
{
    updatePlayerAreaLocked();
    if (const auto it = screens_.find(c.active); it != screens_.end() && screenEligibleLocked(it->second)) return true;
    // A channel is shared, but its sound/playback owner must be a device in the player's current area.
    std::uint32_t replacement = 0;
    double nearest = 1e100;
    for (const auto& [id, s] : screens_) {
        if (s.channel != c.info.id || !screenEligibleLocked(s)) continue;
        const double dx = s.x - lastPlayer_[0], dy = s.y - lastPlayer_[1], dz = s.z - lastPlayer_[2];
        const double distance = dx * dx + dy * dy + dz * dz;
        if (distance < nearest || (distance == nearest && id < replacement)) {
            replacement = id;
            nearest = distance;
        }
    }
    if (!replacement) return false;
    c.active = replacement;
    char id[16];
    std::snprintf(id, sizeof(id), "%08X", replacement);
    logger_.info(tag(c) + "Playback follows screen " + id + " in the player's area.");
    return true;
}

void ScreenController::suspendLocked(Channel& c)
{
    if (c.state == State::Paused && c.resumeOnLoad) return;
    c.resumeAt = positionLocked(c);
    // Invalidate outstanding fetch results and release both file decoders and live ffmpeg processes.
    stopLocked(c, "no loaded screen in the player's area");
    c.state = State::Paused;
    c.resumeOnLoad = true;
    saveSessionLocked(c);
    logger_.info(tag(c) + "Playback suspended outside the player's area; position kept.");
}

void ScreenController::reconcileAudienceLocked()
{
    if (loading_) return;
    for (auto& cp : channels_) {
        auto& c = *cp;
        const bool running = c.state == State::Playing || c.state == State::Fetching;
        const bool waiting = c.resumeOnLoad || (c.sessionPlaying && c.state == State::Idle);
        if (!running && !waiting) continue;
        if (!selectAudienceLocked(c)) {
            if (running) suspendLocked(c);
        } else if (waiting) {
            c.sessionPlaying = false;
            c.resumeOnLoad = false;
            logger_.info(tag(c) + "Playback resumes in the player's area at " + clockText(c.resumeAt) + ".");
            playCurrentLocked(c, c.resumeAt);
        }
    }
}

void ScreenController::showLoadingLocked(Channel& c)
{
    if (!c.loadingCard) {
        c.loadingCard = std::make_shared<TestPatternSource>(c.info.width, c.info.height, 30.0, true);
    }
    setFrameSource(c.info.id, c.loadingCard);
}

void ScreenController::showIdleLocked(Channel& c)
{
    setFrameSource(c.info.id, c.idleCard);
    c.loadingCard.reset();   // its thread ends with it
}

// A media error: TV static (silent) until the next action, under a notice saying what failed.
void ScreenController::showErrorLocked(Channel& c, const std::string& label, const std::string& detail)
{
    setFrameSource(c.info.id, std::make_shared<NoiseSource>(c.info.width, c.info.height));
    c.loadingCard.reset();
    tvNotice(c.info.id, Osd::Icon::Stop, label, detail);
}

std::string ScreenController::playCurrentLocked(Channel& c, double at)
{
    if (!selectAudienceLocked(c)) {
        c.resumeAt = at;
        suspendLocked(c);
        return "Video Screen: waiting for a screen in your area";
    }
    c.resumeOnLoad = false;
    if (playlists_.empty()) {
        return "Video Screen: no playlists";
    }
    Playlist& pl = playlists_[c.playlist];
    const std::uint64_t generation = ++c.generation;
    if (!ensureListedLocked(c, pl, at)) {
        return "Video Screen: loading playlist " + pl.name + "...";
    }
    if (!c.pendingTrackKey.empty()) {   // the session's video, once its playlist is known
        for (std::size_t i = 0; i < pl.tracks.size(); ++i) {
            if (pl.tracks[i].key == c.pendingTrackKey) {
                c.track = i;
            }
        }
        c.pendingTrackKey.clear();
    }
    if (pl.tracks.empty()) {
        c.state = State::Idle;
        showIdleLocked(c);
        return "Video Screen: " + pl.name + " is empty" + (pl.online ? "" : " (add videos to Data\\Video\\Videos)");
    }
    if (c.track >= pl.tracks.size()) {
        c.track = 0;
    }
    const Track track = pl.tracks[c.track];
    if (!pl.online) {
        startFileLocked(c, widen(track.key), at);
        return c.state == State::Playing ? "Playing: " + shorten(track.title, 60) : "Video Screen: cannot play " + shorten(track.title, 60);
    }
    if (!fetcher_->available() && !fetcher_->cachedFile(track.key)) {
        c.state = State::Idle;
        showErrorLocked(c, "NO SIGNAL", "yt-dlp.exe not found");
        return "Video Screen: yt-dlp.exe not found (see ScreenSFSE.ini)";
    }
    if (track.status == "is_live" || track.status == "is_upcoming") {
        streamLocked(c, track);
        return "Live: " + shorten(track.title, 60);
    }
    if (streamedLocked(c, track)) {
        streamLocked(c, track, at);
        return "Streaming: " + shorten(track.title, 60);
    }
    c.state = State::Fetching;
    const bool cached = fetcher_->cachedFile(track.key).has_value();
    showLoadingLocked(c);   // existing downloads may need a one-time compatibility copy too
    const int id = c.info.id;
    fetcher_->request(track.key, [this, id, generation, at, title = track.title](bool ok, const std::filesystem::path& file,
                                                                                 const std::string& error) {
        std::lock_guard lock(mutex_);
        Channel* ch = channelById(id);
        if (!ch || generation != ch->generation || ch->state != State::Fetching) {
            return;   // cancelled or replaced meanwhile
        }
        if (!ok && error == VideoFetcher::kLiveError) {   // listed before it went live: stream it
            if (const Track* t = currentTrackLocked(*ch)) {
                setTrackStatusLocked(t->key, "is_live");
                streamLocked(*ch, *currentTrackLocked(*ch));
            }
            return;
        }
        if (!ok && error == VideoFetcher::kStreamOnlyError) {   // no H.264 MP4 on that site: stream it, now and later
            if (const Track* t = currentTrackLocked(*ch)) {
                setTrackStatusLocked(t->key, "stream");
                streamLocked(*ch, *currentTrackLocked(*ch), at);
            }
            return;
        }
        if (!ok) {
            ch->state = State::Idle;
            showErrorLocked(*ch, "NO SIGNAL", title);
            ch->pendingMessage = "Video Screen: could not load (" + shorten(title, 40) + ") - " + shorten(error, 80);
            return;
        }
        startFileLocked(*ch, file, at);
    }, track.url);
    return (cached ? "Loading: " : "Downloading: ") + shorten(track.title, 60);
}

// Played through LivePlayer on demand, never downloaded: the playlist's stream option, or a site without H.264 MP4s.
bool ScreenController::streamedLocked(const Channel& c, const Track& track) const
{
    return track.status == "stream" || (c.playlist < playlists_.size() && playlists_[c.playlist].streamOnly);
}

void ScreenController::setTrackStatusLocked(const std::string& key, const std::string& status)
{
    for (auto& pl : playlists_) {
        bool changed = false;
        for (auto& t : pl.tracks) {
            if (t.key == key && t.status != status) {
                t.status = status;
                changed = true;
            }
        }
        if (changed && pl.online) {
            saveListCache(pl);
        }
    }
}

// A live (or about to start) video: its HLS URLs from yt-dlp, then LivePlayer. Never downloaded. Not live: the
// video's stream URLs, played on demand from `at`.
void ScreenController::streamLocked(Channel& c, const Track& track, double at)
{
    c.state = State::Fetching;
    showLoadingLocked(c);
    const int id = c.info.id;
    const std::uint64_t generation = c.generation;
    const std::string title = track.title;
    const bool live = track.status == "is_live" || track.status == "is_upcoming";
    fetcher_->requestStream(track.key, [this, id, generation, title, live, at, key = track.key](
                                           bool ok, const std::string& video, const std::string& audio, double duration,
                                           const std::string& headers, const std::string& error) {
        std::lock_guard lock(mutex_);
        Channel* ch = channelById(id);
        if (!ch || generation != ch->generation || ch->state != State::Fetching) {
            return;
        }
        if (!ok && live) {
            ch->state = State::Idle;
            ch->pendingMessage = "Live stream unavailable (" + shorten(title, 40) + ") - " + shorten(error, 80);
            liveFailedLocked(*ch, key, title);
            return;
        }
        if (!ok) {
            ch->state = State::Idle;
            showErrorLocked(*ch, "NO SIGNAL", title);
            ch->pendingMessage = "Video Screen: could not stream (" + shorten(title, 40) + ") - " + shorten(error, 80);
            return;
        }
        startLiveLocked(*ch, video, audio, live ? -1.0 : at, duration, headers);
    }, track.url);
}

// A broadcast ended: channel /live URLs (and some playlists) list a new video ID for the next one. Says so on the
// picture and relists the playlist; refreshListLocked plays the new broadcast if the dead ID is gone from it.
void ScreenController::liveFailedLocked(Channel& c, const std::string& key, const std::string& title)
{
    showErrorLocked(c, "OFF AIR", title);
    if (c.playlist >= playlists_.size() || !playlists_[c.playlist].online) {
        return;
    }
    Playlist& pl = playlists_[c.playlist];
    c.deadLiveKey = key;
    logger_.info(tag(c) + "Live video " + key + " is off the air; relisting " + pl.name + ".");
    pl.refreshed = false;   // an in-flight listing (pl.listing) is used instead
    refreshListLocked(pl);
}

void ScreenController::startLiveLocked(Channel& c, const std::string& videoUrl, const std::string& audioUrl, double startAt,
                                       double duration, const std::string& headers)
{
    const bool onDemand = startAt >= 0.0;
    if (!selectAudienceLocked(c)) { c.resumeAt = onDemand ? startAt : 0.0; suspendLocked(c); return; }
    auto player = std::make_shared<LivePlayer>(logger_, c.info.width, c.info.height, fetcher_->settings().ffmpeg,
                                               config_.liveBufferSeconds);
    if (!player->open(videoUrl, audioUrl, startAt, duration, headers)) {
        c.state = State::Idle;
        showErrorLocked(c, "NO SIGNAL", trackLabelLocked(c));
        c.pendingMessage = "Video Screen: " + player->lastError();
        return;
    }
    c.player = player;
    c.captions.reset();   // streams: no subtitles
    c.subtitleOn = false;
    c.currentFile.clear();
    c.streamVideo = onDemand ? videoUrl : std::string();
    c.streamAudio = onDemand ? audioUrl : std::string();
    c.streamDuration = onDemand ? duration : 0.0;
    c.streamHeaders = onDemand ? headers : std::string();
    c.state = State::Playing;
    c.resumeAt = onDemand ? startAt : 0.0;
    c.started = std::chrono::steady_clock::now();
    updateGainsLocked(c);
    setFrameSource(c.info.id, c.player);
    c.loadingCard.reset();
    saveSessionLocked(c);
    if (onDemand) {
        prefetchNextLocked(c);
    }
    cv_.notify_all();
}

void ScreenController::startFileLocked(Channel& c, const std::filesystem::path& file, double at)
{
    if (!selectAudienceLocked(c)) { c.resumeAt = at; suspendLocked(c); return; }
    auto player = std::make_shared<MediaPlayer>(logger_, c.info.width, c.info.height);
    if (!player->open(file, at)) {
        c.state = State::Idle;
        showErrorLocked(c, "NO SIGNAL", trackLabelLocked(c));
        c.pendingMessage = "Video Screen: " + player->lastError();
        return;
    }
    c.started = std::chrono::steady_clock::now();
    c.player = player;
    c.currentFile = file;
    c.state = State::Playing;
    c.resumeAt = at;
    updateGainsLocked(c);
    c.captions = std::make_shared<SubtitledSource>(player, c.info.width, c.info.height, config_.subtitleSize);
    applySubtitlesLocked(c, true);
    setFrameSource(c.info.id, c.captions);
    c.loadingCard.reset();
    saveSessionLocked(c);
    prefetchNextLocked(c);
    cv_.notify_all();
}

void ScreenController::prefetchNextLocked(const Channel& c)
{
    const Playlist& pl = playlists_[c.playlist];
    if (pl.online && pl.tracks.size() > 1) {
        const Track& next = pl.tracks[(c.track + 1) % pl.tracks.size()];
        if (next.status != "is_live" && next.status != "is_upcoming" && !streamedLocked(c, next)) {
            fetcher_->prefetch(next.key, next.url);
            bool allowUntagged = true;
            const auto languages = subtitleLanguagesLocked(c, allowUntagged);
            if (!languages.empty() && c.subtitleChoice != "off") {
                fetchSubtitlesLocked(next.key, languages, nullptr, next.url);
            }
        }
    }
}

// ---- subtitles ---------------------------------------------------------------------------------------------------

std::vector<std::string> ScreenController::subtitleLanguagesLocked(const Channel& c, bool& allowUntagged) const
{
    allowUntagged = true;
    const std::string option = playlists_.empty() ? std::string() : playlists_[c.playlist].subtitles;
    if (option.empty()) {
        return config_.subtitleLanguages;
    }
    if (lowerPlaylistText(option) == "off") {
        allowUntagged = false;
        return {};
    }
    return splitLanguages(option);
}

std::vector<SubtitleTrack> ScreenController::subtitleTracksLocked(const Channel& c) const
{
    if (playlists_.empty() || c.currentFile.empty()) {
        return {};
    }
    if (playlists_[c.playlist].online) {
        const Track* t = currentTrackLocked(c);
        const auto dir = fetcher_->subtitlesDir();
        return t && !dir.empty() ? Subtitles::find(dir, widen(t->key)) : std::vector<SubtitleTrack>{};
    }
    return Subtitles::find(c.currentFile.parent_path(), c.currentFile.stem().wstring());
}

// Caller holds mutex_. Shows the track chosen for the channel's video (choice, then the languages wanted, then an
// untagged file) on its captions source. fetch: a YouTube video without a cached track in a wanted language asks yt-dlp.
void ScreenController::applySubtitlesLocked(Channel& c, bool fetch)
{
    if (!c.captions) {
        return;
    }
    bool allowUntagged = true;
    const auto languages = subtitleLanguagesLocked(c, allowUntagged);
    const auto tracks = subtitleTracksLocked(c);
    const SubtitleTrack* track = Subtitles::pick(tracks, languages, c.subtitleChoice, allowUntagged);
    if (track) {
        if (track->file != c.subtitleFile || !c.subtitleCues) {
            c.subtitleCues = std::make_shared<const Subtitles::Cues>(Subtitles::load(track->file));
            c.subtitleFile = track->file;
            logger_.info(tag(c) + "Subtitles " + Subtitles::label(track->language) + ": " + narrow(track->file.filename().wstring()) +
                         " (" + std::to_string(c.subtitleCues->size()) + " cues).");
        }
        c.captions->setCues(c.subtitleCues);
        c.subtitleOn = !c.subtitleCues->empty();
        c.subtitleShown = track->language;
    } else {
        c.captions->setCues(nullptr);
        c.subtitleOn = false;
        c.subtitleShown.clear();
        c.subtitleFile.clear();
        c.subtitleCues.reset();
    }
    const Track* t = currentTrackLocked(c);
    if (fetch && t && playlists_[c.playlist].online && !languages.empty() && c.subtitleChoice != "off") {
        const bool cached = std::any_of(tracks.begin(), tracks.end(), [&](const SubtitleTrack& s) {
            return std::any_of(languages.begin(), languages.end(), [&](const std::string& l) { return Subtitles::sameLanguage(s.language, l); });
        });
        if (!cached) {
            fetchSubtitlesLocked(t->key, languages, &c, t->url);
        }
    }
}

// Caller holds mutex_. Once per video and language list per game session; when done, a channel still on that video
// shows them.
void ScreenController::fetchSubtitlesLocked(const std::string& videoId, const std::vector<std::string>& languages, Channel* forChannel,
                                            const std::string& url)
{
    std::string key = videoId + "|";
    for (const auto& l : languages) {
        key += lowerPlaylistText(l) + ",";
    }
    if (!fetcher_->available() || !subtitlesRequested_.insert(key).second) {
        return;
    }
    VideoFetcher::SubtitlesDone done;
    if (forChannel) {
        const int id = forChannel->info.id;
        done = [this, id, videoId](bool found) {
            if (!found) {
                return;
            }
            std::lock_guard lock(mutex_);
            Channel* ch = channelById(id);
            const Track* t = ch ? currentTrackLocked(*ch) : nullptr;
            if (!ch || !ch->captions || !t || t->key != videoId) {
                return;   // another video by now: it finds the files when it plays
            }
            applySubtitlesLocked(*ch, false);
            if (ch->subtitleOn) {
                tvNotice(ch->info.id, Osd::Icon::None, "SUBTITLES " + Subtitles::label(ch->subtitleShown));
            }
        };
    }
    fetcher_->requestSubtitles(videoId, languages, std::move(done), url);
}

std::string ScreenController::cycleSubtitlesLocked(Channel& c)
{
    if (!c.captions || (c.state != State::Playing && c.state != State::Paused)) {
        if (c.player && c.player->live()) {
            return "Live stream: no subtitles";
        }
        if (c.player && !c.streamVideo.empty()) {
            return "Streamed video: no subtitles";
        }
        return "Subtitles: play a video first";
    }
    const auto tracks = subtitleTracksLocked(c);
    if (tracks.empty()) {
        bool allowUntagged = true;
        const auto languages = subtitleLanguagesLocked(c, allowUntagged);
        const Track* t = currentTrackLocked(c);
        if (playlists_[c.playlist].online && t) {
            if (languages.empty()) {
                tvNotice(c.info.id, Osd::Icon::None, "NO SUBTITLES");
                return "No subtitles: set subtitle_languages in ScreenSFSE.ini (e.g. es,en)";
            }
            std::string key = t->key + "|";
            for (const auto& l : languages) {
                key += lowerPlaylistText(l) + ",";
            }
            subtitlesRequested_.erase(key);   // asked by the player: try again
            c.subtitleChoice.clear();
            fetchSubtitlesLocked(t->key, languages, &c, t->url);
            tvNotice(c.info.id, Osd::Icon::None, "SUBTITLES...");
            return "Subtitles: looking for them online";
        }
        tvNotice(c.info.id, Osd::Icon::None, "NO SUBTITLES");
        return "No subtitles for this video (add a .srt file with the video's name)";
    }
    std::size_t current = tracks.size();   // off
    for (std::size_t i = 0; c.subtitleOn && i < tracks.size(); ++i) {
        if (tracks[i].language == c.subtitleShown) {
            current = i;
        }
    }
    const std::size_t next = current == tracks.size() ? 0 : current + 1;
    c.subtitleChoice = next == tracks.size() ? "off" : tracks[next].language.empty() ? "-" : tracks[next].language;
    applySubtitlesLocked(c, false);
    saveSessionLocked(c);
    const std::string label = c.subtitleOn ? Subtitles::label(c.subtitleShown) : std::string("OFF");
    tvNotice(c.info.id, Osd::Icon::None, "SUBTITLES " + label);
    return "Subtitles: " + label;
}

void ScreenController::pauseLocked(Channel& c)
{
    if (c.state == State::Playing && c.player) {
        c.player->pause();
        c.state = State::Paused;
        c.resumeAt = positionLocked(c);
        saveSessionLocked(c);
    }
}

void ScreenController::resumeLocked(Channel& c)
{
    if (!selectAudienceLocked(c)) { suspendLocked(c); return; }
    c.resumeOnLoad = false;
    if (c.state == State::Paused && !c.player) {
        playCurrentLocked(c, c.resumeAt);
        return;
    }
    if (c.state == State::Paused && c.player) {
        c.player->play();
        c.state = State::Playing;
        updateGainsLocked(c);
        saveSessionLocked(c);
        cv_.notify_all();
    }
}

void ScreenController::stopLocked(Channel& c, const char* why)
{
    ++c.generation;
    if (c.state != State::Idle) {
        logger_.info(tag(c) + "Video stopped: " + why);
    }
    c.state = State::Idle;
    c.resumeOnLoad = false;
    showIdleLocked(c);
    c.captions.reset();   // holds the player too; its caption thread ends here
    c.subtitleOn = false;
    c.player.reset();   // the surface released its reference in setFrameSource; this joins the decode threads
    c.streamVideo.clear();
    c.streamAudio.clear();
    c.streamHeaders.clear();
    c.streamDuration = 0.0;
}

double ScreenController::positionLocked(const Channel& c) const
{
    return c.player ? c.player->positionSeconds() : c.resumeAt;
}

void ScreenController::onTrackFinishedLocked(Channel& c)
{
    const bool early = c.player && (std::chrono::steady_clock::now() - c.started < std::chrono::seconds(15) || c.player->stalled());
    if (c.player && c.player->live() && early) {
        // a live stream that fails at once would restart in a loop: stop and say why
        const std::string why = c.player->lastError();
        logger_.info(tag(c) + "Live stream stopped early: " + why);
        stopLocked(c, "live stream failed");
        c.pendingMessage = "Live stream stopped" + (why.empty() ? std::string() : " - " + shorten(why, 100));
        if (const Track* t = currentTrackLocked(c)) {
            liveFailedLocked(c, t->key, t->title);
        }
        saveSessionLocked(c);
        return;
    }
    if (!c.streamVideo.empty() && !c.player->lastError().empty() && early) {
        // an on-demand stream that fails at once: say so, don't run through the playlist
        const std::string why = c.player->lastError();
        logger_.info(tag(c) + "Stream stopped early: " + why);
        const std::string label = trackLabelLocked(c);
        c.resumeAt = positionLocked(c);
        stopLocked(c, "stream failed");
        showErrorLocked(c, "NO SIGNAL", label);
        c.pendingMessage = "Video Screen: the stream stopped - " + shorten(why, 100);
        saveSessionLocked(c);
        return;
    }
    logger_.info(tag(c) + "Video finished: " + trackLabelLocked(c));
    stopLocked(c, "finished");
    Playlist& pl = playlists_[c.playlist];
    if (pl.tracks.empty()) {
        return;
    }
    c.track = (c.track + 1) % pl.tracks.size();   // the playlist loops, like the radio's
    c.resumeAt = 0;
    playCurrentLocked(c, 0);
    tvNotice(c.info.id, Osd::Icon::Play, "PLAY", trackLabelLocked(c));
}

// ---- Papyrus entry points -----------------------------------------------------------------------------------------

void ScreenController::registerScreen(std::uint32_t formId, int channel, float x, float y, float z, float px, float py, float pz,
                                      ScreenLocation::Area area)
{
    std::lock_guard lock(mutex_);
    Channel* c = channelById(channel);
    if (!c) {
        logger_.warn("Screen " + std::to_string(formId) + " registered with unknown channel " + std::to_string(channel) + "; using the first.");
        c = channels_.front().get();
    }
    Screen& s = screens_[formId];
    s.channel = c->info.id;
    s.x = x;
    s.y = y;
    s.z = z;
    s.loaded = true;
    s.area = area;
    lastPlayer_[0] = px;
    lastPlayer_[1] = py;
    lastPlayer_[2] = pz;
    float rx = 0, ry = 0, rz = 0, heading = 0;
    const bool read = readPlayer(rx, ry, rz, heading);
    const float diff = read ? std::sqrt((rx - px) * (rx - px) + (ry - py) * (ry - py) + (rz - pz) * (rz - pz)) : -1.0f;
    const bool ok = read && diff < 100.0f;   // the player can walk a little between the two reads
    if (!playerPositionChecked_ || ok != playerPositionVerified_) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Player position check: Papyrus (%.0f, %.0f, %.0f), direct read %s (%.0f, %.0f, %.0f) -> %s",
                      px, py, pz, read ? "ok" : "FAILED", rx, ry, rz,
                      ok ? "verified, spatial sound follows the player" : "NOT verified, sound is not positional");
        logger_.info(buf);
    }
    playerPositionChecked_ = true;
    playerPositionVerified_ = ok;
    if (loading_) {
        return;   // finishLoad decides what plays once the save's state is known
    }
    reconcileAudienceLocked();
}

std::string ScreenController::status(std::uint32_t formId)
{
    std::lock_guard lock(mutex_);
    Channel* c = channelFor(formId);
    if (!c || playlists_.empty()) {
        return "VIDEO SCREEN";
    }
    const Playlist& pl = playlists_[c->playlist];
    const char* state = c->state == State::Playing ? "PLAYING" : c->state == State::Paused ? "PAUSED"
                                                             : c->state == State::Fetching ? "LOADING" : "STOPPED";
    std::string time = clockText(positionLocked(*c));
    if (c->player && c->player->live()) {
        time = "LIVE " + time;
    } else if (c->player && c->player->durationSeconds() > 0) {
        time += " / " + clockText(c->player->durationSeconds());
    }
    std::string where = pl.name;
    if (pl.listed && !pl.tracks.empty()) {
        where += " " + std::to_string(c->track + 1) + "/" + std::to_string(pl.tracks.size());
    }
    const std::string subtitles = c->subtitleOn ? "  |  Sub " + Subtitles::label(c->subtitleShown) : std::string();
    return "CH " + std::to_string(c->info.number) + "  " + state + "  " + trackLabelLocked(*c) + "  " + time + "  |  Vol " +
           std::to_string(static_cast<int>(c->volume * 100 + 0.5f)) + "%" + subtitles + "  |  " + shorten(where, 40);
}

std::string ScreenController::action(std::uint32_t formId, int act)
{
    std::lock_guard lock(mutex_);
    Channel* cp = channelFor(formId);
    if (!cp) {
        return {};
    }
    Channel& c = *cp;
    if (act == RefreshPlaylists) return refreshPlaylistsLocked(); // validation errors do not alter any channel state
    if (!c.pendingMessage.empty() && c.state == State::Idle && act == PlayPause) {   // e.g. a failed download
        std::string message = std::move(c.pendingMessage);
        c.pendingMessage.clear();
        return message;
    }
    c.pendingMessage.clear();
    if (formId != c.active) {   // the sound moves to the screen just used (same channel, same picture)
        c.active = formId;
        updateGainsLocked(c);
    }
    c.sessionPlaying = false;
    const bool wasActive = c.state != State::Idle;
    Playlist* pl = playlists_.empty() ? nullptr : &playlists_[c.playlist];
    switch (act) {
    case PlayPause:
        switch (c.state) {
        case State::Idle: {
            const std::string m = playCurrentLocked(c, c.resumeAt);
            if (c.state != State::Idle) {
                tvNotice(c.info.id, Osd::Icon::Play, m.rfind("Live", 0) == 0 ? "LIVE" : "PLAY", trackLabelLocked(c));
            }
            return m;
        }
        case State::Fetching:
            stopLocked(c, "cancelled while loading");
            saveSessionLocked(c);
            tvNotice(c.info.id, Osd::Icon::Stop, "STOP");
            return "Video Screen: cancelled";
        case State::Playing:
            c.resumeOnLoad = false;
            pauseLocked(c);
            tvNotice(c.info.id, Osd::Icon::Pause, "PAUSE");
            return "Paused at " + clockText(c.resumeAt);
        case State::Paused:
            resumeLocked(c);
            tvNotice(c.info.id, Osd::Icon::Play, c.player && c.player->live() ? "LIVE" : "PLAY");
            return "Playing: " + trackLabelLocked(c);
        }
        break;
    case NextTrack:
    case PreviousTrack:
        if (!pl) {
            return {};
        }
        if (!pl->listed) {
            ensureListedLocked(c, *pl, 0);
            return "Video Screen: loading playlist " + pl->name + "...";
        }
        if (pl->tracks.empty()) {
            return "Video Screen: " + pl->name + " is empty";
        }
        if (pl->tracks.size() == 1 && wasActive && !(act == PreviousTrack && positionLocked(c) > 5.0)) {
            return pl->name + " has one video. Next Playlist switches lists.";
        }
        if (act == PreviousTrack && positionLocked(c) > 5.0 && wasActive) {
            // like a CD player: back to the start of this video first
        } else if (act == NextTrack) {
            c.track = (c.track + 1) % pl->tracks.size();
        } else {
            c.track = (c.track + pl->tracks.size() - 1) % pl->tracks.size();
        }
        stopLocked(c, act == NextTrack ? "next track" : "previous track");
        c.resumeAt = 0;
        {
            const std::string m = playCurrentLocked(c, 0);
            tvNotice(c.info.id, act == NextTrack ? Osd::Icon::Next : Osd::Icon::Previous,
                     act == NextTrack ? "NEXT" : "PREV", trackLabelLocked(c));
            return m;
        }
    case SeekBack:
    case SeekForward: {
        if (c.player && c.player->live()) {
            return "Live stream: no seeking";
        }
        const double delta = act == SeekForward ? config_.seekSeconds : -config_.seekSeconds;
        double target = std::max(0.0, positionLocked(c) + delta);
        if (c.player && c.player->durationSeconds() > 0) {
            target = std::min(target, std::max(0.0, c.player->durationSeconds() - 2.0));
        }
        if ((c.state == State::Playing || c.state == State::Paused) && !c.currentFile.empty()) {
            const bool paused = c.state == State::Paused;
            const auto file = c.currentFile;
            stopLocked(c, "seek");
            startFileLocked(c, file, target);
            if (paused) {
                pauseLocked(c);
            }
        } else if ((c.state == State::Playing || c.state == State::Paused) && !c.streamVideo.empty()) {
            const bool paused = c.state == State::Paused;
            const auto video = c.streamVideo, audio = c.streamAudio, headers = c.streamHeaders;
            const double duration = c.streamDuration;
            stopLocked(c, "seek");
            startLiveLocked(c, video, audio, target, duration, headers);   // the same URLs from the new position
            if (paused) {
                pauseLocked(c);
            }
        } else {
            c.resumeAt = target;
            saveSessionLocked(c);
        }
        tvNotice(c.info.id, act == SeekForward ? Osd::Icon::Forward : Osd::Icon::Rewind, clockText(target));
        return (act == SeekForward ? "Forward to " : "Back to ") + clockText(target);
    }
    case VolumeUp:
    case VolumeDown:
        c.volume = std::clamp(c.volume + (act == VolumeUp ? config_.volumeStep : -config_.volumeStep), 0.0f, 1.0f);
        c.volume = std::round(c.volume * 100.0f) / 100.0f;
        updateGainsLocked(c);
        saveSessionLocked(c);
        tvNotice(c.info.id, Osd::Icon::Volume, "VOL " + std::to_string(static_cast<int>(c.volume * 100 + 0.5f)), {}, c.volume);
        return "Volume " + std::to_string(static_cast<int>(c.volume * 100 + 0.5f)) + "%";
    case NextPlaylist: {
        if (playlists_.empty()) {
            return {};
        }
        if (wasActive) {
            stopLocked(c, "playlist changed");
        }
        c.playlist = (c.playlist + 1) % playlists_.size();
        c.track = 0;
        c.resumeAt = 0;
        c.pendingTrackKey.clear();
        if (wasActive) {
            const std::string m = playCurrentLocked(c, 0);
            tvNotice(c.info.id, Osd::Icon::Play, playlists_[c.playlist].name, trackLabelLocked(c));
            return "Playlist: " + playlists_[c.playlist].name + " - " + m;
        }
        if (c.state == State::Idle) {
            showIdleLocked(c);   // an error card (static) belongs to the playlist that failed
        }
        saveSessionLocked(c);
        tvNotice(c.info.id, Osd::Icon::None, playlists_[c.playlist].name);
        return "Playlist: " + playlists_[c.playlist].name;
    }
    case Stop:
        stopLocked(c, "stopped by the player");
        c.resumeAt = 0;
        saveSessionLocked(c);
        tvNotice(c.info.id, Osd::Icon::Stop, "STOP");
        return "Video Screen: stopped";
    case CycleSubtitles:
        return cycleSubtitlesLocked(c);
    default:
        break;
    }
    return {};
}

void ScreenController::unloaded(std::uint32_t formId)
{
    std::lock_guard lock(mutex_);
    auto it = screens_.find(formId);
    if (it == screens_.end()) {
        return;
    }
    it->second.loaded = false;
    reconcileAudienceLocked();
}

// ---- the remote ---------------------------------------------------------------------------------------------------

std::string ScreenController::remoteLabelLocked(const Screen& s, float px, float py, float pz)
{
    const Channel* c = channelById(s.channel);
    if (!c) return {};
    const std::string& key = c->info.key;
    const char* model = key.rfind("billboard", 0) == 0 ? "Video Billboard"
                        : key.rfind("small", 0) == 0   ? "Small Video Screen"
                        : key.rfind("tv", 0) == 0      ? "Large Video Screen"
                                                       : "Video Screen";
    const float dx = s.x - px, dy = s.y - py, dz = s.z - pz;
    const float meters = std::sqrt(dx * dx + dy * dy + dz * dz) * kMetersPerUnit;
    const char* state = c->state == State::Playing ? "playing" : c->state == State::Paused ? "paused"
                      : c->state == State::Fetching ? "loading" : "stopped";
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s - CH %d - %.0f m - %s", model, c->info.number, meters, state);
    return buf;
}

bool ScreenController::remoteEligibleLocked(std::uint32_t formId)
{
    updatePlayerAreaLocked();
    const auto it = screens_.find(formId);
    return it != screens_.end() && screenEligibleLocked(it->second);
}

int ScreenController::remoteScreens(float px, float py, float pz)
{
    std::lock_guard lock(mutex_);
    updatePlayerAreaLocked();
    std::vector<std::tuple<float, std::uint32_t, std::string>> found;
    for (const auto& [id, s] : screens_) {
        if (!screenEligibleLocked(s)) continue;
        const float dx = s.x - px, dy = s.y - py, dz = s.z - pz;
        found.emplace_back(dx * dx + dy * dy + dz * dz, id, remoteLabelLocked(s, px, py, pz));
    }
    std::sort(found.begin(), found.end());
    remoteList_.clear();
    for (const auto& [d, id, label] : found) {
        if (static_cast<int>(remoteList_.size()) == kRemoteMax) break;
        remoteList_.emplace_back(id, label);
    }
    return static_cast<int>(remoteList_.size());
}

std::uint32_t ScreenController::remoteScreenId(int index)
{
    std::lock_guard lock(mutex_);
    return index >= 0 && index < static_cast<int>(remoteList_.size()) ? remoteList_[index].first : 0;
}

std::string ScreenController::remoteScreenLabel(int index)
{
    std::lock_guard lock(mutex_);
    return index >= 0 && index < static_cast<int>(remoteList_.size()) ? remoteList_[index].second : std::string();
}

std::string ScreenController::remoteLabel(std::uint32_t formId, float px, float py, float pz)
{
    std::lock_guard lock(mutex_);
    if (!remoteEligibleLocked(formId)) return {};
    return remoteLabelLocked(screens_.find(formId)->second, px, py, pz);
}

std::string ScreenController::remoteField(std::uint32_t formId, int field)
{
    std::lock_guard lock(mutex_);
    if (!remoteEligibleLocked(formId)) return {};
    Channel* c = channelFor(formId);
    if (!c) return {};
    switch (field) {
    case 1: {
        std::string state = c->state == State::Playing ? "Playing" : c->state == State::Paused ? "Paused"
                          : c->state == State::Fetching ? "Loading" : "Stopped";
        return c->player && c->player->live() ? state + " (live)" : state;
    }
    case 2:
        return trackLabelLocked(*c);
    case 3: {
        std::string time = clockText(positionLocked(*c));
        if (c->player && !c->player->live() && c->player->durationSeconds() > 0) {
            time += " / " + clockText(c->player->durationSeconds());
        }
        return time;
    }
    case 4:
        return std::to_string(static_cast<int>(c->volume * 100 + 0.5f)) + "%";
    case 5: {
        if (playlists_.empty()) return "none";
        const Playlist& pl = playlists_[c->playlist];
        std::string where = pl.name;
        if (pl.listed && !pl.tracks.empty()) {
            where += "  (" + std::to_string(c->track + 1) + " of " + std::to_string(pl.tracks.size()) + ")";
        }
        return where;
    }
    case 6:
        return c->subtitleOn ? Subtitles::label(c->subtitleShown) : "Off";
    default:
        return {};
    }
}

std::string ScreenController::remoteAction(std::uint32_t formId, int act)
{
    {
        std::lock_guard lock(mutex_);
        if (!remoteEligibleLocked(formId)) return "Video Screen Remote: that screen is not in this area";
    }
    g_noticeScale = 2;
    std::string message = action(formId, act);
    g_noticeScale = 1;
    return message;
}

bool ScreenController::musicMuteWanted(float playerX, float playerY, float playerZ)
{
    std::lock_guard lock(mutex_);
    reconcileAudienceLocked();
    lastPlayer_[0] = playerX;
    lastPlayer_[1] = playerY;
    lastPlayer_[2] = playerZ;
    std::string why;
    for (const auto& cp : channels_) {
        const Channel& c = *cp;
        if (!config_.muteGameMusic || (c.state != State::Playing && c.state != State::Fetching)) {
            continue;
        }
        const auto it = screens_.find(c.active);
        if (it == screens_.end() || !screenEligibleLocked(it->second)) {
            continue;
        }
        const float dx = it->second.x - playerX, dy = it->second.y - playerY, dz = it->second.z - playerZ;
        const float meters = std::sqrt(dx * dx + dy * dy + dz * dz) * kMetersPerUnit;
        if (meters < config_.silentMeters * c.info.soundRange) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s plays %.0f m away", c.info.key.c_str(), meters);
            why = buf;
            break;
        }
    }
    const int wanted = why.empty() ? 0 : 1;
    if (wanted != lastMusicMute_) {
        logger_.info(wanted ? "Game music: silenced (" + why + ")." : std::string("Game music: back (no screen playing nearby)."));
        lastMusicMute_ = wanted;
    }
    return wanted == 1;
}

float ScreenController::soundSourceRange(std::uint32_t formId)
{
    std::lock_guard lock(mutex_);
    const auto it = screens_.find(formId);
    Channel* c = it != screens_.end() ? channelById(it->second.channel) : nullptr;
    if (!config_.muteGameMusic || !c || c->active != formId || (c->state != State::Playing && c->state != State::Fetching)) {
        return 0.0f;
    }
    return config_.silentMeters * c->info.soundRange / kMetersPerUnit;
}

void ScreenController::removed(std::uint32_t formId)
{
    std::lock_guard lock(mutex_);
    auto it = screens_.find(formId);
    if (it == screens_.end()) {
        return;
    }
    Channel* c = channelById(it->second.channel);
    if (c && formId == c->active) {
        it->second.loaded = false;
        if (!selectAudienceLocked(*c)) {
            c->resumeAt = positionLocked(*c);
            stopLocked(*c, "its screen was removed");
        }
        saveSessionLocked(*c);
    }
    screens_.erase(it);
}

// ---- spatial sound + housekeeping (only while something plays) ---------------------------------------------------

// Caller holds mutex_.
void ScreenController::updateGainsLocked(Channel& c)
{
    if (!c.player) {
        return;
    }
    const auto it = screens_.find(c.active);
    if (it == screens_.end() || !screenEligibleLocked(it->second)) {
        c.player->setGains(0, 0);
        return;
    }
    float px = lastPlayer_[0], py = lastPlayer_[1], pz = lastPlayer_[2], heading = 0;
    bool facing = false;
    if (playerPositionVerified_) {
        facing = readPlayer(px, py, pz, heading);
    }
    const Screen& s = it->second;
    const float dx = s.x - px, dy = s.y - py, dz = s.z - pz;
    const float meters = std::sqrt(dx * dx + dy * dy + dz * dz) * kMetersPerUnit;
    const float silent = config_.silentMeters * c.info.soundRange, full = config_.fullVolumeMeters * c.info.soundRange;
    float gain = 1.0f;
    if (meters >= silent) {
        gain = 0.0f;
    } else if (meters > full) {
        const float t = (silent - meters) / (silent - full);
        gain = t * t;
    }
    gain *= c.volume;
    float left = gain, right = gain;
    if (facing) {
        // heading: radians clockwise from +Y; pan +1 = screen on the right. Centred when standing at the screen.
        const float bearing = std::atan2(dx, dy);
        const float pan = std::sin(bearing - heading) * std::min(1.0f, meters / std::max(0.1f, full));
        const float theta = (pan + 1.0f) * kPi / 4.0f;
        left = gain * std::min(1.0f, std::cos(theta) * 1.41421356f);
        right = gain * std::min(1.0f, std::sin(theta) * 1.41421356f);
    }
    c.player->setGains(left, right);
}

void ScreenController::spatialLoop()
{
    std::unique_lock lock(mutex_);
    auto anyPlaying = [this] {
        return std::any_of(channels_.begin(), channels_.end(), [](const auto& c) { return c->state == State::Playing; });
    };
    while (!quit_) {
        cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return quit_ || anyPlaying(); });
        if (quit_) {
            break;
        }
        reconcileAudienceLocked();
        const auto now = std::chrono::steady_clock::now();
        for (auto& cp : channels_) {
            Channel& c = *cp;
            if (c.state != State::Playing || !c.player) {
                continue;
            }
            if (c.player->finished()) {
                onTrackFinishedLocked(c);
                continue;
            }
            updateGainsLocked(c);
            if (now - c.lastSessionSave >= std::chrono::seconds(5)) {
                c.resumeAt = positionLocked(c);
                saveSessionLocked(c);
            }
        }
        cv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return quit_; });
    }
}
