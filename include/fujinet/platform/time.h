#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace fujinet::platform {

// Returns current UNIX time in seconds (UTC). 0 if unknown/not set.
std::uint64_t unix_time_seconds();

// Best-effort set UNIX time (UTC). Returns true if applied.
bool set_unix_time_seconds(std::uint64_t secs);

// True if we believe the clock is valid (i.e. not "unset").
bool time_is_valid();

// Synchronize time from network (NTP). Returns true if sync was successful.
// On ESP32: triggers immediate SNTP sync.
// On POSIX: typically a no-op (system time already managed by OS).
bool sync_network_time();

// Format UTC unix seconds into "YYYY-MM-DDTHH:MM:SSZ". Returns false if unavailable.
bool format_time_utc_iso8601(std::uint64_t unix_seconds, char* out, std::size_t out_len);

// Format UTC unix seconds into "Mon dd HH:MM" (ls-style). Returns false if unavailable.
bool format_time_utc_ls(std::uint64_t unix_seconds, char* out, std::size_t out_len);

// ============================================================================
// Timezone Support
// ============================================================================

// Local time components
struct LocalTime {
    int year;      // e.g., 2024
    int month;     // 1-12
    int day;       // 1-31
    int hour;      // 0-23
    int minute;    // 0-59
    int second;    // 0-59
    int weekday;   // 0-6 (Sunday-Saturday)
    int yearday;   // 0-365
    bool is_dst;   // daylight saving time flag
};

// Set the system timezone (POSIX format, e.g., "UTC" or "CET-1CEST,M3.5.0,M10.5.0/3")
// Returns true if the timezone was set successfully.
bool set_timezone(const char* posix_tz);

// Get the current system timezone string.
std::string get_timezone();

// Validate a timezone string. Returns true if valid.
bool validate_timezone(const char* posix_tz);

// Get local time components for a specific timezone without affecting system timezone.
// Returns true on success, false if timezone is invalid or time unavailable.
bool get_local_time(std::uint64_t unix_seconds, const char* tz, LocalTime& out);

// Format time with timezone applied into ISO 8601 format: "YYYY-MM-DDTHH:MM:SS+HHMM"
// Returns false if unavailable or timezone invalid.
bool format_time_local_iso8601(std::uint64_t unix_seconds, const char* tz, char* out, std::size_t out_len);

/**
 * @brief RAII helper to temporarily set a timezone and restore it after use.
 * 
 * This is used internally by the timezone functions to avoid affecting
 * the system timezone when getting/formatting time for a specific timezone.
 */
class ScopedTimezone {
public:
    explicit ScopedTimezone(const char* new_tz);
    ~ScopedTimezone();
    
    // Non-copyable, non-movable
    ScopedTimezone(const ScopedTimezone&) = delete;
    ScopedTimezone& operator=(const ScopedTimezone&) = delete;
    ScopedTimezone(ScopedTimezone&&) = delete;
    ScopedTimezone& operator=(ScopedTimezone&&) = delete;
    
private:
    std::string old_tz_;
    bool had_tz_;
};

} // namespace fujinet::platform
