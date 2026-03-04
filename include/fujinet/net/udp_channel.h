#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/net/udp_socket_ops.h"

#include <string>
#include <memory>

namespace fujinet::net {

// Common UDP channel implementation that uses the platform-agnostic socket operations interface.
class UdpChannel final : public fujinet::io::Channel {
public:
    explicit UdpChannel(IUdpSocketOps& socket_ops, const std::string& host, uint16_t port);
    ~UdpChannel() override;

    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t max_len) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;

private:
    IUdpSocketOps& socket_ops_;
    std::string host_;
    uint16_t port_;
    int socket_fd_;
    bool connected_;
};

} // namespace fujinet::net
