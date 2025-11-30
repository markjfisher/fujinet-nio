// include/fujinet/platform/esp32/channel_factory.h
#pragma once

#include <memory>
#include "fujinet/io/core/channel.h"

namespace fujinet::platform::esp32 {

std::unique_ptr<fujinet::io::Channel> create_default_channel();

} // namespace fujinet::platform::esp32