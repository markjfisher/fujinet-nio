#include "fujinet/platform/time.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_sntp.h"
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

    // This sets the system "wall clock" used by newlib time(),
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

bool sync_network_time()
{
    // ESP32: Trigger immediate SNTP sync
    // This will request time from NTP servers and update system time
    
    // Check if SNTP is already running
    if (!esp_sntp_enabled()) {
        ESP_LOGW(TAG, "SNTP not enabled, cannot sync time");
        return false;
    }
    
    // Request immediate sync
    esp_sntp_restart();
    
    ESP_LOGI(TAG, "SNTP time sync requested");
    
    // Wait a short time for sync (up to 1 second)
    // The actual sync happens asynchronously
    int retries = 10;
    while (retries > 0) {
        if (time_is_valid()) {
            ESP_LOGI(TAG, "Time sync successful");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        retries--;
    }
    
    // Even if we didn't get a valid time yet, the sync is in progress
    // Return true to indicate the request was made
    return true;
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

// ============================================================================
// Timezone Support
// ============================================================================

bool set_timezone(const char* posix_tz)
{
    if (!posix_tz || posix_tz[0] == '\0') {
        return false;
    }
    ::setenv("TZ", posix_tz, 1);
    ::tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", posix_tz);
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
    ScopedTimezone scoped_tz(posix_tz);
    
    time_t now = ::time(nullptr);
    tm local_tm{};
    
    if (::localtime_r(&now, &local_tm) == nullptr) {
        return false;
    }
    
    // If we got here, the timezone was accepted
    return true;
}

bool get_local_time(std::uint64_t unix_seconds, const char* tz, LocalTime& out)
{
    if (!tz || tz[0] == '\0') {
        return false;
    }
    
    ScopedTimezone scoped_tz(tz);
    
    const time_t t = static_cast<time_t>(unix_seconds);
    tm local_tm{};
    
    if (::localtime_r(&t, &local_tm) == nullptr) {
        return false;
    }
    
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
    
    const time_t t = static_cast<time_t>(unix_seconds);
    tm local_tm{};
    
    if (::localtime_r(&t, &local_tm) == nullptr) {
        return false;
    }
    
    // Format: YYYY-MM-DDTHH:MM:SS+HHMM
    return ::strftime(out, out_len, "%FT%T%z", &local_tm) != 0;
}

} // namespace fujinet::platform
