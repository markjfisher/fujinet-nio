#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/net/tcp_socket_ops.h"

#include <string>
#include <memory>

namespace fujinet::net {

// Common TCP channel implementation that uses the platform-agnostic socket operations interface.
class TcpChannel final : public fujinet::io::Channel {
public:
    explicit TcpChannel(ITcpSocketOps& socket_ops, const std::string& host, uint16_t port);
    ~TcpChannel() override;

    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t max_len) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;

private:
    ITcpSocketOps& socket_ops_;
    std::string host_;
    uint16_t port_;
    int socket_fd_;
    bool connected_;
};

} // namespace fujinet::net
