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
    const void* tcp_stream_addrinfo_hints() const noexcept override
    {
        static struct addrinfo hints {};
        static bool inited = false;
        if (!inited) {
            hints = {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            inited = true;
        }
        return &hints;
    }

    const void* tcp_stream_passive_addrinfo_hints() const noexcept override
    {
        static struct addrinfo hints {};
        static bool inited = false;
        if (!inited) {
            hints = {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_flags = AI_PASSIVE;
            inited = true;
        }
        return &hints;
    }

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

    int bind(int fd, const struct sockaddr* addr, SockLen addrlen) override
    {
        return lwip_bind(fd, addr, static_cast<socklen_t>(addrlen));
    }

    int listen(int fd, int backlog) override
    {
        return lwip_listen(fd, backlog);
    }

    int accept(int fd, struct sockaddr* addr, SockLen* inout_addrlen) override
    {
        socklen_t alen = 0;
        socklen_t* p = nullptr;
        if (addr && inout_addrlen) {
            alen = static_cast<socklen_t>(*inout_addrlen);
            p = &alen;
        }
        const int cfd = lwip_accept(fd, addr, p);
        if (cfd >= 0 && inout_addrlen && p) {
            *inout_addrlen = static_cast<SockLen>(alen);
        }
        return cfd;
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

    SSize send(int fd, const void* buf, std::size_t len) override
    {
        // ESP-IDF lwIP: MSG_DONTWAIT for nonblocking
        // Note: MSG_NOSIGNAL not available on lwIP
        return lwip_send(fd, buf, len, MSG_DONTWAIT);
    }

    SSize recv(int fd, void* buf, std::size_t len) override
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

    void apply_stream_socket_options(int fd, bool nodelay, bool keepalive) override
    {
        if (fd < 0) return;
        if (nodelay) {
            int v = 1;
            (void)lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, static_cast<socklen_t>(sizeof(v)));
        }
        if (keepalive) {
            int v = 1;
            (void)lwip_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &v, static_cast<socklen_t>(sizeof(v)));
        }
    }

    void apply_listen_socket_options(int fd) override
    {
        if (fd < 0) return;
        int v = 1;
        (void)lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, static_cast<socklen_t>(sizeof(v)));
    }

    int getaddrinfo(const char* host, const char* port, const void* hints, AddrInfo** out) override
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

    bool is_would_block(int errno_val) const noexcept override
    {
        return errno_val == EAGAIN || errno_val == EWOULDBLOCK;
    }

    bool is_in_progress(int errno_val) const noexcept override
    {
        return errno_val == EINPROGRESS;
    }

    bool is_peer_gone(int errno_val) const noexcept override
    {
        return errno_val == ECONNRESET || errno_val == ENOTCONN || errno_val == EPIPE;
    }

    int err_timed_out() const noexcept override
    {
        return ETIMEDOUT;
    }

    int err_conn_refused() const noexcept override
    {
        return ECONNREFUSED;
    }

    int err_host_unreach() const noexcept override
    {
        return EHOSTUNREACH;
    }
};

// Global instance for ESP32 platform
static EspIdfTcpSocketOps g_espidf_socket_ops;

ITcpSocketOps& get_espidf_socket_ops()
{
    return g_espidf_socket_ops;
}

} // namespace fujinet::net
