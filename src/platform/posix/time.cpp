#include "fujinet/platform/time.h"

#include <ctime>
#include <cstdint>
#include <cstdio>

#if defined(__unix__) || defined(__APPLE__)
#include <time.h>
#endif

namespace fujinet::platform {

std::uint64_t unix_time_seconds()
{
    std::time_t t = std::time(nullptr);
    if (t <= 0) return 0;
    return static_cast<std::uint64_t>(t);
}

bool set_unix_time_seconds(std::uint64_t secs)
{
// #if defined(__unix__) || defined(__APPLE__)
//     // Optional: only if running as root / has CAP_SYS_TIME.
//     timespec ts{};
//     ts.tv_sec = static_cast<time_t>(secs);
//     ts.tv_nsec = 0;
//     return ::clock_settime(CLOCK_REALTIME, &ts) == 0;
// #else
//     (void)secs;
//     return false;
// #endif
    // Don't actually try and set the time in posix, assume it's correct already
    (void)secs;
    return false;
}

bool time_is_valid()
{
    // Anything after 2020-01-01 is “valid enough”
    return unix_time_seconds() >= 1577836800ULL;
}

static bool gmtime_utc(std::uint64_t unix_seconds, tm& out_tm)
{
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    tm tm{};

#if defined(__unix__) || defined(__APPLE__)
    if (::gmtime_r(&t, &tm) == nullptr) {
        return false;
    }
#else
    // Best-effort fallback if gmtime_r is unavailable.
    tm* p = std::gmtime(&t);
    if (!p) return false;
    tm = *p;
#endif

    out_tm = tm;
    return true;
}

bool format_time_utc_iso8601(std::uint64_t unix_seconds, char* out, std::size_t out_len)
{
    if (!out || out_len == 0 || unix_seconds == 0) return false;
    tm tm{};
    if (!gmtime_utc(unix_seconds, tm)) return false;
    return ::strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm) != 0;
}

bool format_time_utc_ls(std::uint64_t unix_seconds, char* out, std::size_t out_len)
{
    if (!out || out_len == 0 || unix_seconds == 0) return false;
    tm tm{};
    if (!gmtime_utc(unix_seconds, tm)) return false;
    return ::strftime(out, out_len, "%b %e %H:%M", &tm) != 0;
}

} // namespace fujinet::platform
