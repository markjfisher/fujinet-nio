#pragma once

#include "fujinet/hotsync/link.h"

namespace fujinet::net {
class ITcpSocketOps;
}

namespace fujinet::hotsync {

// A Link over a connected, non-blocking socket. Owns the socket and closes it.
// Waits by polling, so it belongs on the HotSync task, never the core loop.
class TcpLink : public Link {
public:
    TcpLink(net::ITcpSocketOps& ops, int fd);
    ~TcpLink() override;

    TcpLink(const TcpLink&) = delete;
    TcpLink& operator=(const TcpLink&) = delete;

    int read(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs) override;
    int write(const std::uint8_t* buf, std::size_t len) override;

private:
    net::ITcpSocketOps& _ops;
    int _fd;
};

} // namespace fujinet::hotsync
