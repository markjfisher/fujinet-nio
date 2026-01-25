#include "fujinet/core/logging.h"

#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <ctime>

namespace fujinet::log {

void early_logf(const char* fmt, ...)
{
    if (!fmt) {
        return;
    }

    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

#if !defined(FN_DEBUG)

// Non-debug build: nothing else here. Inline stubs in the header handle log calls.

#else

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

#if defined(FN_DEBUG_LOG_TS)
static void print_timestamp(FILE* out)
{
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto tt = system_clock::to_time_t(now);

    std::tm tm{};
    localtime_r(&tt, &tm);

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);

    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::fprintf(out, "%s.%03ld ", buf, static_cast<long>(ms.count()));
}
#endif

static void print_prefix(FILE* out, Level level, const char* tag)
{
#if defined(FN_DEBUG_LOG_TS)
    print_timestamp(out);
#endif
    std::fprintf(out, "[%s] %s: ", level_to_str(level), tag ? tag : "log");
}

void vlogf(Level level, const char* tag, const char* fmt, std::va_list args)
{
    FILE* out = (level == Level::Error || level == Level::Warn)
        ? stderr
        : stdout;

    print_prefix(out, level, tag);
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

    print_prefix(out, level, tag);
    std::fprintf(out, "%.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
}

#endif // defined(FN_DEBUG)

} // namespace fujinet::log
