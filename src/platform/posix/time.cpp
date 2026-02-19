#include "fujinet/platform/time.h"

#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// ============================================================================
// Timezone Support
// ============================================================================

// Helper class to temporarily set a timezone and restore it after use
class ScopedTimezone {
    std::string old_tz_;
    bool had_tz_;
public:
    explicit ScopedTimezone(const char* new_tz) {
        const char* old = std::getenv("TZ");
        had_tz_ = (old != nullptr);
        if (had_tz_) {
            old_tz_ = old;
        }
        ::setenv("TZ", new_tz, 1);
        ::tzset();
    }
    
    ~ScopedTimezone() {
        if (had_tz_) {
            ::setenv("TZ", old_tz_.c_str(), 1);
        } else {
            ::unsetenv("TZ");
        }
        ::tzset();
    }
};

bool set_timezone(const char* posix_tz)
{
    if (!posix_tz || posix_tz[0] == '\0') {
        return false;
    }
    ::setenv("TZ", posix_tz, 1);
    ::tzset();
    return true;
}

std::string get_timezone()
{
    const char* tz = std::getenv("TZ");
    return tz ? std::string(tz) : std::string("UTC");
}

bool validate_timezone(const char* posix_tz)
{
    if (!posix_tz || posix_tz[0] == '\0') {
        return false;
    }
    
    // Try to apply the timezone temporarily and see if it works
    // by checking if localtime produces valid results
    ScopedTimezone scoped_tz(posix_tz);
    
    std::time_t now = std::time(nullptr);
    tm local_tm{};
    
#if defined(__unix__) || defined(__APPLE__)
    if (::localtime_r(&now, &local_tm) == nullptr) {
        return false;
    }
#else
    tm* p = std::localtime(&now);
    if (!p) return false;
    local_tm = *p;
#endif
    
    // If we got here, the timezone was accepted
    return true;
}

bool get_local_time(std::uint64_t unix_seconds, const char* tz, LocalTime& out)
{
    if (!tz || tz[0] == '\0') {
        return false;
    }
    
    ScopedTimezone scoped_tz(tz);
    
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    tm local_tm{};
    
#if defined(__unix__) || defined(__APPLE__)
    if (::localtime_r(&t, &local_tm) == nullptr) {
        return false;
    }
#else
    tm* p = std::localtime(&t);
    if (!p) return false;
    local_tm = *p;
#endif
    
    out.year = local_tm.tm_year + 1900;
    out.month = local_tm.tm_mon + 1;
    out.day = local_tm.tm_mday;
    out.hour = local_tm.tm_hour;
    out.minute = local_tm.tm_min;
    out.second = local_tm.tm_sec;
    out.weekday = local_tm.tm_wday;
    out.yearday = local_tm.tm_yday;
    out.is_dst = local_tm.tm_isdst > 0;
    
    return true;
}

bool format_time_local_iso8601(std::uint64_t unix_seconds, const char* tz, char* out, std::size_t out_len)
{
    if (!out || out_len == 0 || unix_seconds == 0 || !tz || tz[0] == '\0') {
        return false;
    }
    
    ScopedTimezone scoped_tz(tz);
    
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    tm local_tm{};
    
#if defined(__unix__) || defined(__APPLE__)
    if (::localtime_r(&t, &local_tm) == nullptr) {
        return false;
    }
#else
    tm* p = std::localtime(&t);
    if (!p) return false;
    local_tm = *p;
#endif
    
    // Format: YYYY-MM-DDTHH:MM:SS+HHMM
    return ::strftime(out, out_len, "%FT%T%z", &local_tm) != 0;
}

} // namespace fujinet::platform
