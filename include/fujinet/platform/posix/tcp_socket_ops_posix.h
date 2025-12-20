#pragma once

#include "fujinet/net/tcp_socket_ops.h"

namespace fujinet::net {

// Get the POSIX socket operations implementation.
ITcpSocketOps& get_posix_socket_ops();

} // namespace fujinet::net

