#include "fujinet/platform/posix/udp_channel.h"
#include "fujinet/net/udp_channel.h"
#include "fujinet/platform/udp_socket_ops.h"

namespace fujinet::platform {

std::unique_ptr<fujinet::io::Channel> create_udp_channel(const std::string& host, uint16_t port) {
    return std::make_unique<fujinet::net::UdpChannel>(default_udp_socket_ops(), host, port);
}

}  // namespace fujinet::platform
