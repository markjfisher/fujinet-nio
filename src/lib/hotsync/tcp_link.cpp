#include "fujinet/hotsync/tcp_link.h"

#include "fujinet/net/tcp_socket_ops.h"

#include <chrono>
#include <thread>

namespace fujinet::hotsync {

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(2);
// A peer that accepts no data for this long is gone.
constexpr std::uint64_t kWriteTimeoutMs = 10000;

} // namespace

TcpLink::TcpLink(net::ITcpSocketOps& ops, int fd)
    : _ops(ops), _fd(fd)
{
}

TcpLink::~TcpLink()
{
    if (_fd >= 0) {
        _ops.close(_fd);
    }
}

int TcpLink::read(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs)
{
    if (_fd < 0) {
        return -1;
    }
    const std::uint64_t deadline = _ops.now_ms() + timeoutMs;
    for (;;) {
        const net::SSize got = _ops.recv(_fd, buf, len);
        if (got > 0) {
            return static_cast<int>(got);
        }
        if (got == 0 || !_ops.is_would_block(_ops.last_errno())) {
            return -1; // closed by the peer, or a socket error
        }
        if (_ops.now_ms() >= deadline) {
            return 0;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

int TcpLink::write(const std::uint8_t* buf, std::size_t len)
{
    if (_fd < 0) {
        return -1;
    }
    std::uint64_t deadline = _ops.now_ms() + kWriteTimeoutMs;
    std::size_t done = 0;
    while (done < len) {
        const net::SSize sent = _ops.send(_fd, buf + done, len - done);
        if (sent > 0) {
            done += static_cast<std::size_t>(sent);
            deadline = _ops.now_ms() + kWriteTimeoutMs;
            continue;
        }
        if (sent < 0 && !_ops.is_would_block(_ops.last_errno())) {
            return -1;
        }
        if (_ops.now_ms() >= deadline) {
            return -1;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return static_cast<int>(len);
}

} // namespace fujinet::hotsync
