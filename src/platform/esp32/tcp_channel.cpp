#include "fujinet/platform/esp32/tcp_channel.h"
#include "fujinet/net/tcp_channel.h"
#include "fujinet/platform/tcp_socket_ops.h"

namespace fujinet::platform {

std::unique_ptr<fujinet::io::Channel> create_tcp_channel(const std::string& host, uint16_t port) {
    return std::make_unique<fujinet::net::TcpChannel>(default_tcp_socket_ops(), host, port);
}

}  // namespace fujinet::platform
