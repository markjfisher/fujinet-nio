#pragma once

#include "fujinet/net/tcp_socket_ops.h"

namespace fujinet::platform {

// Return the platform's default TCP socket operations implementation.
// Implemented in platform-specific .cpp (POSIX / ESP32).
fujinet::net::ITcpSocketOps& default_tcp_socket_ops();

} // namespace fujinet::platform


