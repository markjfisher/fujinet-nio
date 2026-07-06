#pragma once

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/channel.h"

#include <memory>

namespace fujinet::platform::esp32 {

std::unique_ptr<fujinet::io::Channel>
create_atari_sio_fujibus_channel(const fujinet::config::FujiConfig& config);

} // namespace fujinet::platform::esp32
