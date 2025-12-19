#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace fujinet::core {

/**
 * Central hub that hosts the individual streams.
 *
 * Keep it small and explicit: add streams as the project needs them.
 */
class EventHub {
public:
    EventHub() = default;

    // Streams are defined in their own headers (net/time etc) to avoid cross-layer coupling.
    // This file only provides the mechanism; see fujinet/net/network_events.h etc.

private:
    // Intentionally empty: streams live as members in a concrete hub type
    // We keep EventHub as a container-free utility for now.
};

} // namespace fujinet::core
