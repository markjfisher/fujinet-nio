#pragma once

#include "fujinet/net/udp_socket_ops.h"

namespace fujinet::net {

// Get the POSIX socket operations implementation for UDP.
IUdpSocketOps& get_posix_udp_socket_ops();

} // namespace fujinet::net
