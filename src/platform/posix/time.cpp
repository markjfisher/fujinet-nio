#include "fujinet/platform/time.h"

#include <ctime>
#include <cstdint>

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

} // namespace fujinet::platform
