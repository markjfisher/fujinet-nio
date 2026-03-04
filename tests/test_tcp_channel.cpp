#include "doctest.h"
#include <fujinet/io/core/channel.h>
#include <fujinet/platform/tcp_socket_ops.h>
#include <fujinet/net/tcp_channel.h>

#include <string>

using namespace fujinet;

TEST_CASE("TCP Channel - create") {
    auto& socket_ops = platform::default_tcp_socket_ops();
    auto channel = std::make_unique<net::TcpChannel>(socket_ops, "localhost", 12345);
    
    // Check that the channel was created successfully
    CHECK(channel != nullptr);
}

TEST_CASE("TCP Channel - available initially false") {
    auto& socket_ops = platform::default_tcp_socket_ops();
    auto channel = std::make_unique<net::TcpChannel>(socket_ops, "localhost", 12345);
    
    CHECK(!channel->available());
}
