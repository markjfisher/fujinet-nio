#include "fujinet/platform/udp_socket_ops.h"

#include "fujinet/platform/esp32/udp_socket_ops_espidf.h"

namespace fujinet::platform {

fujinet::net::IUdpSocketOps& default_udp_socket_ops()
{
    return fujinet::net::get_esp32_udp_socket_ops();
}

} // namespace fujinet::platform
