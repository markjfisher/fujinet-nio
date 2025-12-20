#pragma once

#include "fujinet/net/tcp_socket_ops.h"

namespace fujinet::net {

// Get the ESP-IDF socket operations implementation.
ITcpSocketOps& get_espidf_socket_ops();

} // namespace fujinet::net

