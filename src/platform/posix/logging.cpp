#include "fujinet/core/logging.h"

#if !defined(FN_DEBUG)

// Non-debug build: nothing here. Inline stubs in the header handle calls.

#else

#include <cstdarg>
#include <cstdio>

namespace fujinet::log {

static const char* level_to_str(Level lvl)
{
    switch (lvl) {
    case Level::Error:   return "E";
    case Level::Warn:    return "W";
    case Level::Info:    return "I";
    case Level::Debug:   return "D";
    case Level::Verbose: return "V";
    }
    return "?";
}

void vlogf(Level level, const char* tag, const char* fmt, std::va_list args)
{
    FILE* out = (level == Level::Error || level == Level::Warn)
        ? stderr
        : stdout;

    std::fprintf(out, "[%s] %s: ", level_to_str(level), tag ? tag : "log");
    std::vfprintf(out, fmt, args);
    std::fprintf(out, "\n");
}

void logf(Level level, const char* tag, const char* fmt, ...)
{
    std::va_list args;
    va_start(args, fmt);
    vlogf(level, tag, fmt, args);
    va_end(args);
}

void log(Level level, const char* tag, std::string_view message)
{
    FILE* out = (level == Level::Error || level == Level::Warn)
        ? stderr
        : stdout;

    std::fprintf(out, "[%s] %s: %.*s\n",
                 level_to_str(level),
                 tag ? tag : "log",
                 static_cast<int>(message.size()),
                 message.data());
}

} // namespace fujinet::log

#endif // !defined(FN_DEBUG)