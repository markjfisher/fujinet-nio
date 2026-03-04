#pragma once

#include "fujinet/net/udp_socket_ops.h"

namespace fujinet::net {

// Get the ESP-IDF socket operations implementation for UDP.
IUdpSocketOps& get_esp32_udp_socket_ops();

} // namespace fujinet::net
