#include "fujinet/platform/time.h"

#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "sys/time.h"
#include <time.h>

namespace fujinet::platform {

static const char* TAG = "platform";

std::uint64_t unix_time_seconds()
{
    time_t now{};
    ::time(&now);
    if (now <= 0) return 0;
    return static_cast<std::uint64_t>(now);
}

bool set_unix_time_seconds(std::uint64_t secs)
{
    timeval tv{};
    tv.tv_sec  = static_cast<time_t>(secs);
    tv.tv_usec = 0;

    // This sets the system “wall clock” used by newlib time(),
    // and typically what FAT/VFS uses for timestamps.
    int rc = ::settimeofday(&tv, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
        return false;
    }

    ESP_LOGI(TAG, "System time set to %llu", static_cast<unsigned long long>(secs));
    return true;
}

bool time_is_valid()
{
    // after start of 2020 is good enough check.
    return unix_time_seconds() >= 1577836800ULL;
}

static bool gmtime_utc(std::uint64_t unix_seconds, tm& out_tm)
{
    const time_t t = static_cast<time_t>(unix_seconds);
    tm tmp{};
    if (::gmtime_r(&t, &tmp) == nullptr) {
        return false;
    }
    out_tm = tmp;
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
    // ls-style: "Jan  7 22:59" (12 chars)
    return ::strftime(out, out_len, "%b %e %H:%M", &tm) != 0;
}

} // namespace fujinet::platform
