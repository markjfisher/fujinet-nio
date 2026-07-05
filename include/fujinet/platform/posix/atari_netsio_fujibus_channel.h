#pragma once

#include "fujinet/io/core/channel.h"

#include <memory>

namespace fujinet::platform::posix {

std::unique_ptr<fujinet::io::Channel>
create_atari_netsio_fujibus_channel(std::unique_ptr<fujinet::io::Channel> udp);

} // namespace fujinet::platform::posix
