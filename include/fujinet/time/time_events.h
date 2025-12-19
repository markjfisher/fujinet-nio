#pragma once

#include <cstdint>
#include <string>

namespace fujinet::time {

enum class TimeEventKind : std::uint8_t {
    Synchronized,
    ManuallySet,
};

struct TimeSynchronized {
    std::string source; // "sntp", "manual", etc.
};

struct TimeEvent {
    TimeEventKind kind{TimeEventKind::ManuallySet};
    TimeSynchronized synced{};
};

} // namespace fujinet::time
