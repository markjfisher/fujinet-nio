#pragma once

#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"

namespace fujinet::config {

// The configuration a fresh device starts from: FujiConfig's own defaults, adjusted
// for what the build profile knows about its platform: the boot disk's URI, the TCP
// port, and the UART's rate and pacing. The config store starts from this, so a value
// written in fujinet.yaml still wins.
FujiConfig default_config_for(const build::BuildProfile& profile);

} // namespace fujinet::config
