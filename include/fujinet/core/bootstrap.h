#pragma once

#include "fujinet/core/core.h"
#include "fujinet/build/profile.h"
#include "fujinet/io/core/channel.h"

namespace fujinet::config {
    struct FujiConfig;
}

namespace fujinet::io {
    class IBusLineCoordinator;
}

namespace fujinet::core {

// Create transports appropriate for this build profile and platform,
// attach them to the core, and return (optionally) the primary transport.
// Config is required for transports that need it (e.g., NetSIO).
// lineCoordinator, when given, shares the FujiBus SLIP transport's line with
// something else (the Palm cradle: hotsync::HotSyncRuntime); other transports
// do not take it.
io::ITransport* setup_transports(FujinetCore& core,
                                 io::Channel& channel,
                                 const build::BuildProfile& profile,
                                 const config::FujiConfig* config = nullptr,
                                 io::IBusLineCoordinator* lineCoordinator = nullptr);

} // namespace fujinet::core
