// Builds without HotSync (FN_WITH_HOTSYNC off): start_hotsync() does nothing.
// The ESP32 build compiles every source, so this file compiles to nothing in
// HotSync builds; src/lib/hotsync/hotsync_init.cpp is the real one.
#if FN_WITH_HOTSYNC != 1

#include "fujinet/hotsync/hotsync_init.h"

#include "fujinet/io/core/channel.h"

namespace fujinet::hotsync {

std::unique_ptr<HotSyncRuntime> start_hotsync(const HotSyncHost&, std::unique_ptr<io::Channel>&)
{
    return nullptr;
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
