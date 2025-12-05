#include "fujinet/build/profile.h"

namespace fujinet::build {

HardwareCapabilities detect_hardware_capabilities()
{
    HardwareCapabilities caps{};

    // POSIX always has networking (via host OS)
    caps.network.hasLocalNetwork = true;
    caps.network.managesItsOwnLink = false;
    caps.network.supportsAccessPointMode = false;

    // Treat POSIX storage as "effectively infinite"
    caps.memory.persistentStorageBytes = std::size_t(-1);

    // Likewise RAM is effectively unbounded for our purposes
    caps.memory.largeMemoryPoolBytes = std::size_t(-1);
    caps.memory.hasDedicatedLargePool = false;

    // USB handled by OS; FujiNet doesn't own the stack
    caps.hasUsbDevice = false;
    caps.hasUsbHost   = false;

    return caps;
}

} // namespace fujinet::build
