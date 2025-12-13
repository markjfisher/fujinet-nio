#include "fujinet/platform/time.h"

#include <cstdint>

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

} // namespace fujinet::platform
