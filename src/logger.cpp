#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <shlobj.h>

namespace
{
std::filesystem::path fallbackLogPath()
{
    return std::filesystem::path("Data") / "SFSE" / "Plugins" / "ScreenSFSE.log";
}

std::string toLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
}

bool Logger::initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);

    logPath_ = resolveLogPath();
    std::error_code ec;
    std::filesystem::create_directories(logPath_.parent_path(), ec);

    stream_.open(logPath_, std::ios::out | std::ios::app);
    if (!stream_.is_open()) {
        logPath_ = fallbackLogPath();
        std::filesystem::create_directories(logPath_.parent_path(), ec);
        stream_.open(logPath_, std::ios::out | std::ios::app);
    }

    if (!stream_.is_open()) {
        return false;
    }

    if (static_cast<int>(Level::Info) >= static_cast<int>(minimumLevel_)) {
        stream_ << '[' << timestamp() << "] [INFO] Logger initialized.\n";
        stream_.flush();
    }
    return true;
}

void Logger::setLevel(Level level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    minimumLevel_ = level;
}

bool Logger::setLevelFromString(const std::string& levelText)
{
    const std::string lowered = toLowerAscii(levelText);
    if (lowered == "info") {
        setLevel(Level::Info);
        return true;
    }
    if (lowered == "warn" || lowered == "warning") {
        setLevel(Level::Warn);
        return true;
    }
    if (lowered == "error" || lowered == "quiet") {
        setLevel(Level::Error);
        return true;
    }
    return false;
}

Logger::Level Logger::level() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return minimumLevel_;
}

void Logger::info(const std::string& message)
{
    write(Level::Info, "INFO", message);
}

void Logger::warn(const std::string& message)
{
    write(Level::Warn, "WARN", message);
}

void Logger::error(const std::string& message)
{
    write(Level::Error, "ERROR", message);
}

std::filesystem::path Logger::path() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return logPath_;
}

void Logger::write(Level level, const char* levelLabel, const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_.is_open()) {
        return;
    }
    if (static_cast<int>(level) < static_cast<int>(minimumLevel_)) {
        return;
    }

    stream_ << '[' << timestamp() << "] [" << levelLabel << "] " << message << '\n';
    stream_.flush();
}

std::filesystem::path Logger::resolveLogPath() const
{
    PWSTR documentsRaw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documentsRaw);
    if (FAILED(hr) || documentsRaw == nullptr) {
        if (documentsRaw != nullptr) {
            CoTaskMemFree(documentsRaw);
        }
        return fallbackLogPath();
    }

    const std::filesystem::path base(documentsRaw);
    CoTaskMemFree(documentsRaw);

    return base / "My Games" / "Starfield" / "SFSE" / "Logs" / "ScreenSFSE.log";
}

std::string Logger::timestamp()
{
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now();
    const std::time_t tt = Clock::to_time_t(now);

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}
