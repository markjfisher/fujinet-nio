#pragma once

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/build/profile.h"

namespace fujinet::config {
    struct FujiConfig;
}

namespace fujinet::platform {

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const build::BuildProfile& profile, const config::FujiConfig& config);

} // namespace fujinet::platform
