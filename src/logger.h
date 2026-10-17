#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

class Logger
{
public:
    enum class Level
    {
        Info = 0,
        Warn = 1,
        Error = 2
    };

    bool initialize();
    void setLevel(Level level);
    bool setLevelFromString(const std::string& levelText);
    Level level() const;
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);

    std::filesystem::path path() const;

private:
    void write(Level level, const char* levelLabel, const std::string& message);
    std::filesystem::path resolveLogPath() const;
    static std::string timestamp();

    mutable std::mutex mutex_;
    std::ofstream stream_;
    std::filesystem::path logPath_;
    Level minimumLevel_{ Level::Warn };
};
