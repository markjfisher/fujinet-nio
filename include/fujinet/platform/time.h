#pragma once
#include <cstdint>

namespace fujinet::platform {

// Returns current UNIX time in seconds (UTC). 0 if unknown/not set.
std::uint64_t unix_time_seconds();

// Best-effort set UNIX time (UTC). Returns true if applied.
bool set_unix_time_seconds(std::uint64_t secs);

// True if we believe the clock is valid (i.e. not “unset”).
bool time_is_valid();

} // namespace fujinet::platform
