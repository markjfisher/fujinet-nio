#pragma once
#include <cstdint>
#include <cstddef>

namespace fujinet::platform {

// Returns current UNIX time in seconds (UTC). 0 if unknown/not set.
std::uint64_t unix_time_seconds();

// Best-effort set UNIX time (UTC). Returns true if applied.
bool set_unix_time_seconds(std::uint64_t secs);

// True if we believe the clock is valid (i.e. not “unset”).
bool time_is_valid();

// Format UTC unix seconds into "YYYY-MM-DDTHH:MM:SSZ". Returns false if unavailable.
bool format_time_utc_iso8601(std::uint64_t unix_seconds, char* out, std::size_t out_len);

// Format UTC unix seconds into "Mon dd HH:MM" (ls-style). Returns false if unavailable.
bool format_time_utc_ls(std::uint64_t unix_seconds, char* out, std::size_t out_len);

} // namespace fujinet::platform
