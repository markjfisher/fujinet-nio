#include "fujinet/platform/udp_socket_ops.h"

#include "fujinet/platform/posix/udp_socket_ops_posix.h"

namespace fujinet::platform {

fujinet::net::IUdpSocketOps& default_udp_socket_ops()
{
    return fujinet::net::get_posix_udp_socket_ops();
}

} // namespace fujinet::platform
