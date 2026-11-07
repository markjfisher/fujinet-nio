#include "fujinet/platform/posix/tcp_server_channel.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fujinet::platform::posix {

class TcpServerChannel : public fujinet::io::Channel {
public:
    TcpServerChannel(std::string host, std::uint16_t port, bool readableWait)
        : _host(std::move(host))
        , _port(port)
        , _readableWait(readableWait)
    {
        open_listener();
    }

    ~TcpServerChannel() override
    {
        close_client();
        if (_listenFd >= 0) {
            ::close(_listenFd);
        }
    }

    bool available() override
    {
        ensure_client();
        if (_clientFd < 0) {
            return false;
        }

        pollfd pfd{};
        pfd.fd = _clientFd;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        const int ret = ::poll(&pfd, 1, 0);
        if (ret <= 0) {
            return false;
        }
        /* POLLIN and POLLHUP may arrive together when a peer has sent its
         * final bytes then half-closed.  Keep that data available for the
         * transport to drain before treating the connection as closed. */
        if ((pfd.revents & (POLLHUP | POLLERR)) && !(pfd.revents & POLLIN)) {
            close_client();
            return false;
        }
        return (pfd.revents & POLLIN) != 0;
    }

    std::uint32_t connection_epoch() const override { return _epoch; }

    bool supports_readable_wait() const override { return _readableWait; }

    bool wait_for_readable(std::chrono::milliseconds timeout) override
    {
        ensure_client();
        pollfd pfd{};
        pfd.fd = _clientFd >= 0 ? _clientFd : _listenFd;
        pfd.events = POLLIN;
        if (pfd.fd < 0) {
            return false;
        }
        return ::poll(&pfd, 1, static_cast<int>(timeout.count())) > 0;
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        ensure_client();
        if (_clientFd < 0) {
            return 0;
        }

        const ssize_t n = ::read(_clientFd, buffer, maxLen);
        if (n > 0) {
            return static_cast<std::size_t>(n);
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close_client();
        }
        return 0;
    }

    void write(const std::uint8_t* buffer, std::size_t len) override
    {
        ensure_client();
        if (_clientFd < 0) {
            return;
        }

        const std::uint8_t* ptr = buffer;
        std::size_t remaining = len;
        while (remaining > 0) {
            const ssize_t n = ::write(_clientFd, ptr, remaining);
            if (n > 0) {
                remaining -= static_cast<std::size_t>(n);
                ptr += n;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // The peer is not reading. Dropping the rest leaves a framed peer
                // out of step, so say so rather than fail silently.
                std::cerr << "[TcpServerChannel] Send buffer full: dropped " << remaining
                          << " of " << len << " bytes" << std::endl;
                break;
            }
            close_client();
            break;
        }
    }

private:
    static bool set_nonblocking(int fd)
    {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    void open_listener()
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        addrinfo* res = nullptr;
        const std::string portStr = std::to_string(_port);
        const char* host = _host.empty() ? nullptr : _host.c_str();
        const int gai = ::getaddrinfo(host, portStr.c_str(), &hints, &res);
        if (gai != 0) {
            std::cerr << "[TcpServerChannel] getaddrinfo failed for "
                      << (_host.empty() ? "*" : _host) << ":" << _port
                      << ": " << ::gai_strerror(gai) << std::endl;
            return;
        }

        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) {
                continue;
            }

            int yes = 1;
            (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

            if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, 1) == 0) {
                if (set_nonblocking(fd)) {
                    _listenFd = fd;
                    break;
                }
            }
            ::close(fd);
        }

        ::freeaddrinfo(res);

        if (_listenFd >= 0) {
            std::cout << "[TcpServerChannel] Listening on "
                      << (_host.empty() ? "*" : _host) << ":" << _port << std::endl;
        } else {
            std::cerr << "[TcpServerChannel] Failed to listen on "
                      << (_host.empty() ? "*" : _host) << ":" << _port
                      << ": " << std::strerror(errno) << std::endl;
        }
    }

    void ensure_client()
    {
        if (_clientFd >= 0 || _listenFd < 0) {
            return;
        }

        sockaddr_storage addr{};
        socklen_t addrLen = sizeof(addr);
        const int fd = ::accept(_listenFd, reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (fd < 0) {
            return;
        }

        if (!set_nonblocking(fd)) {
            ::close(fd);
            return;
        }

        _clientFd = fd;
        ++_epoch;
        std::cout << "[TcpServerChannel] Client connected" << std::endl;
    }

    void close_client()
    {
        if (_clientFd >= 0) {
            ::close(_clientFd);
            _clientFd = -1;
            ++_epoch;
            std::cout << "[TcpServerChannel] Client disconnected" << std::endl;
        }
    }

    std::string _host;
    std::uint16_t _port;
    bool _readableWait;
    int _listenFd{-1};
    int _clientFd{-1};
    std::uint32_t _epoch{0};
};

std::unique_ptr<fujinet::io::Channel>
create_tcp_server_channel(const std::string& host, std::uint16_t port, bool readableWait)
{
    return std::make_unique<TcpServerChannel>(host, port, readableWait);
}

} // namespace fujinet::platform::posix
