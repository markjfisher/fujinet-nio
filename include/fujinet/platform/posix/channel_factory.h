#pragma once

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/config/build_profile.h"

namespace fujinet::platform::posix {

// Create a Channel appropriate for the given build profile.
std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const config::BuildProfile& profile);

} // namespace fujinet::platform::posix
