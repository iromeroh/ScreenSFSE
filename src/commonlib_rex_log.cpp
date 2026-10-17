#include "REX/LOG.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
const char* levelToString(const REX::ELogLevel level)
{
    switch (level) {
    case REX::ELogLevel::Trace:
        return "TRACE";
    case REX::ELogLevel::Debug:
        return "DEBUG";
    case REX::ELogLevel::Info:
        return "INFO";
    case REX::ELogLevel::Warning:
        return "WARN";
    case REX::ELogLevel::Error:
        return "ERROR";
    case REX::ELogLevel::Critical:
        return "CRITICAL";
    default:
        return "LOG";
    }
}
}

namespace REX::Impl
{
void Log(const std::source_location a_loc, const ELogLevel a_level, const std::string_view a_fmt)
{
    std::fprintf(
        stderr,
        "[CommonLib:%s] %s:%u %s\n",
        levelToString(a_level),
        a_loc.file_name(),
        static_cast<unsigned>(a_loc.line()),
        std::string(a_fmt).c_str());
}

void Log(const std::source_location a_loc, const ELogLevel a_level, const std::wstring_view a_fmt)
{
    std::string narrow;
    narrow.reserve(a_fmt.size());
    for (const wchar_t ch : a_fmt) {
        if (ch >= 0 && ch <= 0x7F) {
            narrow.push_back(static_cast<char>(ch));
        } else {
            narrow.push_back('?');
        }
    }
    Log(a_loc, a_level, narrow);
}

void Fail(const std::source_location a_loc, const std::string_view a_fmt)
{
    Log(a_loc, ELogLevel::Critical, a_fmt);
    std::abort();
}

void Fail(const std::source_location a_loc, const std::wstring_view a_fmt)
{
    Log(a_loc, ELogLevel::Critical, a_fmt);
    std::abort();
}
}
