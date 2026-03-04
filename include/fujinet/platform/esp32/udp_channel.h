#pragma once

#include "fujinet/io/core/channel.h"

#include <memory>
#include <string>

namespace fujinet::platform {

std::unique_ptr<fujinet::io::Channel> create_udp_channel(const std::string& host, uint16_t port);

}  // namespace fujinet::platform
