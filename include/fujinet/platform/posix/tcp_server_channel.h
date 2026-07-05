#pragma once

#include "fujinet/io/core/channel.h"

#include <cstdint>
#include <memory>
#include <string>

namespace fujinet::platform::posix {

std::unique_ptr<fujinet::io::Channel>
create_tcp_server_channel(const std::string& host, std::uint16_t port);

} // namespace fujinet::platform::posix
