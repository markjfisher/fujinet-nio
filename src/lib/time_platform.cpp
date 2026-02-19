/**
 * @file time_platform.cpp
 * @brief Common platform time implementation for timezone support.
 * 
 * This file contains the common implementation of timezone utilities
 * that are shared between POSIX and ESP32 platforms.
 */

#include "fujinet/platform/time.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

namespace fujinet::platform {

ScopedTimezone::ScopedTimezone(const char* new_tz)
    : had_tz_(false)
{
    // Save the old TZ value
    const char* old_tz_env = std::getenv("TZ");
    had_tz_ = (old_tz_env != nullptr);
    if (had_tz_) {
        old_tz_ = old_tz_env;
    }
    
    // Set the new timezone
    ::setenv("TZ", new_tz, 1);
    ::tzset();
}

ScopedTimezone::~ScopedTimezone()
{
    // Restore the old timezone
    if (had_tz_) {
        ::setenv("TZ", old_tz_.c_str(), 1);
    } else {
        ::unsetenv("TZ");
    }
    ::tzset();
}

} // namespace fujinet::platform
