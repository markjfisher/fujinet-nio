#include "fujinet/platform/tcp_socket_ops.h"

#include "fujinet/platform/posix/tcp_socket_ops_posix.h"

namespace fujinet::platform {

fujinet::net::ITcpSocketOps& default_tcp_socket_ops()
{
    return fujinet::net::get_posix_socket_ops();
}

} // namespace fujinet::platform


