#pragma once

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/channel.h"

#include <memory>

namespace fujinet::platform::posix {

std::unique_ptr<fujinet::io::Channel> create_pty_channel(const config::FujiConfig& config);

} // namespace fujinet::platform::posix
