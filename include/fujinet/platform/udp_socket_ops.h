#pragma once

#include "fujinet/net/udp_socket_ops.h"

namespace fujinet::platform {

// Return the platform's default UDP socket operations implementation.
// Implemented in platform-specific .cpp (POSIX / ESP32).
fujinet::net::IUdpSocketOps& default_udp_socket_ops();

} // namespace fujinet::platform
