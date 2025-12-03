#pragma once

#include "fujinet/core/core.h"
#include "fujinet/build/profile.h"
#include "fujinet/io/core/channel.h"

namespace fujinet::core {

// Create transports appropriate for this build profile and platform,
// attach them to the core, and return (optionally) the primary transport.
io::ITransport* setup_transports(FujinetCore& core,
                                 io::Channel& channel,
                                 const build::BuildProfile& profile);

} // namespace fujinet::core
