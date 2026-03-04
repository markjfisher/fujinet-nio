#include "fujinet/platform/esp32/udp_socket_ops_espidf.h"

#include <lwip/err.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>

#include <cstring>

namespace fujinet::net {

class Esp32UdpSocketOps final : public IUdpSocketOps {
public:
    int socket(int domain, int type, int protocol) override {
        return ::socket(domain, type, protocol);
    }

    void close(int fd) override {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    int connect(int fd, const struct sockaddr* addr, SockLen addrlen) override {
        return ::connect(fd, addr, static_cast<socklen_t>(addrlen));
    }

    int set_nonblocking(int fd) override {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            return -1;
        }
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    SSize send(int fd, const void* buf, std::size_t len) override {
        return ::send(fd, buf, len, 0);
    }

    SSize sendto(int fd, const void* buf, std::size_t len, const struct sockaddr* addr, SockLen addrlen) override {
        return ::sendto(fd, buf, len, 0, addr, static_cast<socklen_t>(addrlen));
    }

    SSize recv(int fd, void* buf, std::size_t len) override {
        return ::recv(fd, buf, len, 0);
    }

    SSize recvfrom(int fd, void* buf, std::size_t len, struct sockaddr* addr, SockLen* addrlen) override {
        return ::recvfrom(fd, buf, len, 0, addr, reinterpret_cast<socklen_t*>(addrlen));
    }

    bool poll_readable(int fd) override {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        int ret = select(fd + 1, &read_fds, nullptr, nullptr, &tv);
        return ret > 0;
    }

    int getaddrinfo(const char* host, const char* port, const void* hints, AddrInfo** out) override {
        return ::getaddrinfo(host, port, static_cast<const struct addrinfo*>(hints),
                           reinterpret_cast<struct addrinfo**>(out));
    }

    const void* udp_addrinfo_hints() const noexcept override {
        static const struct addrinfo hints = {
            AI_PASSIVE,    // ai_flags
            AF_UNSPEC,     // ai_family (IPv4 or IPv6)
            SOCK_DGRAM,    // ai_socktype (UDP)
            IPPROTO_UDP,   // ai_protocol (UDP)
            0,             // ai_addrlen
            nullptr,       // ai_addr
            nullptr,       // ai_canonname
            nullptr        // ai_next
        };
        return &hints;
    }

    void freeaddrinfo(AddrInfo* ai) override {
        ::freeaddrinfo(reinterpret_cast<struct addrinfo*>(ai));
    }

    AddrInfo* addrinfo_next(AddrInfo* ai) override {
        return reinterpret_cast<AddrInfo*>(reinterpret_cast<struct addrinfo*>(ai)->ai_next);
    }

    int addrinfo_family(AddrInfo* ai) override {
        return reinterpret_cast<struct addrinfo*>(ai)->ai_family;
    }

    int addrinfo_socktype(AddrInfo* ai) override {
        return reinterpret_cast<struct addrinfo*>(ai)->ai_socktype;
    }

    int addrinfo_protocol(AddrInfo* ai) override {
        return reinterpret_cast<struct addrinfo*>(ai)->ai_protocol;
    }

    const struct sockaddr* addrinfo_addr(AddrInfo* ai, SockLen* out_len) override {
        auto* ainfo = reinterpret_cast<struct addrinfo*>(ai);
        *out_len = static_cast<SockLen>(ainfo->ai_addrlen);
        return ainfo->ai_addr;
    }

    int last_errno() override {
        return errno;
    }

    const char* err_string(int errno_val) override {
        return strerror(errno_val);
    }

    bool is_would_block(int errno_val) const noexcept override {
        return errno_val == EAGAIN || errno_val == EWOULDBLOCK;
    }
};

static Esp32UdpSocketOps s_esp32_udp_socket_ops;

IUdpSocketOps& get_esp32_udp_socket_ops() {
    return s_esp32_udp_socket_ops;
}

} // namespace fujinet::net
