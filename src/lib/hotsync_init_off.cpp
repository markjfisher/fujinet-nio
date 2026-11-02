// start_hotsync() for builds without HotSync. The build selects this file
// instead of src/lib/hotsync/hotsync_init.cpp when FN_WITH_HOTSYNC is off.

#include "fujinet/hotsync/hotsync_init.h"

#include "fujinet/io/core/channel.h"

namespace fujinet::hotsync {

std::unique_ptr<HotSyncRuntime> start_hotsync(const HotSyncHost&, std::unique_ptr<io::Channel>&)
{
    return nullptr;
}

} // namespace fujinet::hotsync
