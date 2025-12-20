#include "fujinet/net/tcp_socket_ops.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace fujinet::net {

class PosixTcpSocketOps final : public ITcpSocketOps {
public:
    int socket(int domain, int type, int protocol) override
    {
        return ::socket(domain, type, protocol);
    }

    void close(int fd) override
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    int connect(int fd, const struct sockaddr* addr, socklen_t addrlen) override
    {
        return ::connect(fd, addr, addrlen);
    }

    int set_nonblocking(int fd) override
    {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0) return -1;
        return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    bool poll_connect_complete(int fd) override
    {
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLOUT;

        const int pr = ::poll(&pfd, 1, 0);
        if (pr < 0) {
            return false; // error, errno set
        }
        return pr > 0; // true if ready, false if still connecting
    }

    ssize_t send(int fd, const void* buf, size_t len, int flags) override
    {
        return ::send(fd, buf, len, flags);
    }

    ssize_t recv(int fd, void* buf, size_t len, int flags) override
    {
        return ::recv(fd, buf, len, flags);
    }

    int shutdown_write(int fd) override
    {
        return ::shutdown(fd, SHUT_WR);
    }

    int get_so_error(int fd) override
    {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
            return errno;
        }
        return err;
    }

    int setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) override
    {
        return ::setsockopt(fd, level, optname, optval, optlen);
    }

    std::uint64_t now_ms() override
    {
        struct timespec ts {};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        const std::uint64_t ms =
            static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL +
            static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ULL;
        return ms;
    }

    int last_errno() override
    {
        return errno;
    }

    const char* err_string(int errno_val) override
    {
        return std::strerror(errno_val);
    }
};

// Global instance for POSIX platform
static PosixTcpSocketOps g_posix_socket_ops;

ITcpSocketOps& get_posix_socket_ops()
{
    return g_posix_socket_ops;
}

} // namespace fujinet::net

