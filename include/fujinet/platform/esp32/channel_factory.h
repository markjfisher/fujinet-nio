#pragma once

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/config/build_profile.h"

namespace fujinet::platform::esp32 {

// Create a Channel appropriate for the given build profile.
// For now, this will be a UART-backed channel (UART0 or another UART).
std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const config::BuildProfile& profile);

} // namespace fujinet::platform::esp32
