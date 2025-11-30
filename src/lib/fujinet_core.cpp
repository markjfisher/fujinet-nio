#include "fujinet/core/core.h"

namespace fujinet::core {

void FujinetCore::tick()
{
    // Later:
    //  - IOService::serviceOnce()
    //  - deviceManager.pollDevices()
    //  - timers, etc.
    //
    // For now, just count ticks.
    ++_tickCount;
}

} // namespace fujinet::core
