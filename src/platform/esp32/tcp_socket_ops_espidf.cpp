#include "fujinet/net/tcp_socket_ops.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>        // F_GETFL, F_SETFL, O_NONBLOCK
#include <netinet/tcp.h>  // TCP_NODELAY (sockopt level IPPROTO_TCP)

extern "C" {
#include "lwip/netdb.h"    // lwip_getaddrinfo, lwip_freeaddrinfo, addrinfo
#include "lwip/sockets.h"  // lwip_socket/connect/send/recv/select/close/setsockopt/getsockopt/shutdown/fcntl
#include "esp_timer.h"     // esp_timer_get_time
}

namespace fujinet::net {

class EspIdfTcpSocketOps final : public ITcpSocketOps {
public:
    int socket(int domain, int type, int protocol) override
    {
        return lwip_socket(domain, type, protocol);
    }

    void close(int fd) override
    {
        if (fd >= 0) {
            lwip_close(fd);
        }
    }

    int connect(int fd, const struct sockaddr* addr, SockLen addrlen) override
    {
        return lwip_connect(fd, addr, static_cast<socklen_t>(addrlen));
    }

    int set_nonblocking(int fd) override
    {
        const int flags = lwip_fcntl(fd, F_GETFL, 0);
        if (flags < 0) return -1;
        return lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    bool poll_connect_complete(int fd) override
    {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);

        timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        const int sr = lwip_select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (sr < 0) {
            return false; // error, errno set
        }
        return sr > 0; // true if ready, false if still connecting
    }

    SSize send(int fd, const void* buf, std::size_t len, int flags) override
    {
        // ESP-IDF lwIP: MSG_DONTWAIT for nonblocking
        // Note: MSG_NOSIGNAL not available on lwIP
        return lwip_send(fd, buf, len, MSG_DONTWAIT);
    }

    SSize recv(int fd, void* buf, std::size_t len, int flags) override
    {
        // ESP-IDF lwIP: MSG_DONTWAIT for nonblocking
        return lwip_recv(fd, buf, len, MSG_DONTWAIT);
    }

    int shutdown_write(int fd) override
    {
        return lwip_shutdown(fd, SHUT_WR);
    }

    int get_so_error(int fd) override
    {
        int err = 0;
        socklen_t len = sizeof(err);
        if (lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
            return errno;
        }
        return err;
    }

    int setsockopt(int fd, int level, int optname, const void* optval, SockLen optlen) override
    {
        return lwip_setsockopt(fd, level, optname, optval, static_cast<socklen_t>(optlen));
    }

    int getaddrinfo(const char* host, const char* port, void* hints, AddrInfo** out) override
    {
        struct addrinfo* res = nullptr;
        const int gai = lwip_getaddrinfo(host, port, static_cast<const struct addrinfo*>(hints), &res);
        if (gai == 0) {
            *out = reinterpret_cast<AddrInfo*>(res);
        } else {
            *out = nullptr;
        }
        return gai;
    }

    void freeaddrinfo(AddrInfo* ai) override
    {
        if (ai) {
            lwip_freeaddrinfo(reinterpret_cast<struct addrinfo*>(ai));
        }
    }

    AddrInfo* addrinfo_next(AddrInfo* ai) override
    {
        if (!ai) return nullptr;
        return reinterpret_cast<AddrInfo*>(reinterpret_cast<struct addrinfo*>(ai)->ai_next);
    }

    int addrinfo_family(AddrInfo* ai) override
    {
        if (!ai) return 0;
        return reinterpret_cast<const struct addrinfo*>(ai)->ai_family;
    }

    int addrinfo_socktype(AddrInfo* ai) override
    {
        if (!ai) return 0;
        return reinterpret_cast<const struct addrinfo*>(ai)->ai_socktype;
    }

    int addrinfo_protocol(AddrInfo* ai) override
    {
        if (!ai) return 0;
        return reinterpret_cast<const struct addrinfo*>(ai)->ai_protocol;
    }

    const struct sockaddr* addrinfo_addr(AddrInfo* ai, SockLen* out_len) override
    {
        if (!ai) return nullptr;
        const struct addrinfo* a = reinterpret_cast<const struct addrinfo*>(ai);
        if (out_len) {
            *out_len = static_cast<SockLen>(a->ai_addrlen);
        }
        return a->ai_addr;
    }

    std::uint64_t now_ms() override
    {
        // esp_timer_get_time() returns microseconds
        return static_cast<std::uint64_t>(esp_timer_get_time() / 1000ULL);
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

// Global instance for ESP32 platform
static EspIdfTcpSocketOps g_espidf_socket_ops;

ITcpSocketOps& get_espidf_socket_ops()
{
    return g_espidf_socket_ops;
}

} // namespace fujinet::net
