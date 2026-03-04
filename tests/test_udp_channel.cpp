#include "doctest.h"
#include <fujinet/io/core/channel.h>
#include <fujinet/platform/udp_socket_ops.h>
#include <fujinet/net/udp_channel.h>

#include <string>

using namespace fujinet;

TEST_CASE("UDP Channel - create") {
    auto& socket_ops = platform::default_udp_socket_ops();
    auto channel = std::make_unique<net::UdpChannel>(socket_ops, "localhost", 12345);
    
    // Check that the channel was created successfully
    CHECK(channel != nullptr);
}

TEST_CASE("UDP Channel - available initially false") {
    auto& socket_ops = platform::default_udp_socket_ops();
    auto channel = std::make_unique<net::UdpChannel>(socket_ops, "localhost", 12345);
    
    CHECK(!channel->available());
}
