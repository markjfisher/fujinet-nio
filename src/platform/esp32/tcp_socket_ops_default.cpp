#include "fujinet/platform/tcp_socket_ops.h"

#include "fujinet/platform/esp32/tcp_socket_ops_espidf.h"

namespace fujinet::platform {

fujinet::net::ITcpSocketOps& default_tcp_socket_ops()
{
    return fujinet::net::get_espidf_socket_ops();
}

} // namespace fujinet::platform


