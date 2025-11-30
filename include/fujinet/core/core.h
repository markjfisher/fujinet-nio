#pragma once

#include <cstdint>

namespace fujinet::core {

// Core “engine” for the I/O system.
// For now it just tracks a tick counter; later this will:
//  - pump IOService
//  - poll devices
//  - handle timers, etc.
class FujinetCore {
public:
    FujinetCore() = default;

    // One iteration of the core loop.
    // Call this regularly from POSIX main or a FreeRTOS task.
    void tick();

    // How many ticks have been executed so far.
    std::uint64_t tick_count() const noexcept { return _tickCount; }

private:
    std::uint64_t _tickCount{0};
};

} // namespace fujinet::core
