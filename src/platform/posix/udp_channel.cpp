#include "fujinet/platform/channel_factory.h"

#include <memory>
#include <cstdio>
#include <string>
#include <cstring>
#include <cerrno>

#include "fujinet/io/core/channel.h"
#include "fujinet/build/profile.h"
#include "fujinet/core/logging.h"

#if !defined(_WIN32)

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace fujinet::platform {

static constexpr const char* TAG = "udp";

static void format_hex_prefix(const std::uint8_t* buffer, std::size_t len, char* out, std::size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    std::size_t pos = 0;
    out[0] = '\0';

    const std::size_t n = (len < 16) ? len : 16;
    for (std::size_t i = 0; i < n; ++i) {
        int wrote = std::snprintf(out + pos, out_len - pos, "%02X%s", buffer[i], (i + 1 == n) ? "" : " ");
        if (wrote <= 0) {
            break;
        }
        pos += static_cast<std::size_t>(wrote);
        if (pos >= out_len) {
            out[out_len - 1] = '\0';
            break;
        }
    }
}

// Forward declaration for factory function
std::unique_ptr<fujinet::io::Channel> create_udp_channel(const std::string& host, std::uint16_t port);

class UdpChannel : public fujinet::io::Channel {
public:
    UdpChannel(const std::string& host, std::uint16_t port)
        : _host(host)
        , _port(port)
        , _socketFd(-1)
        , _connected(false)
    {
        _socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (_socketFd < 0) {
            FN_LOGE(TAG, "Failed to create UDP socket: %s", std::strerror(errno));
            return;
        }

        // Set non-blocking
        int flags = ::fcntl(_socketFd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(_socketFd, F_SETFL, flags | O_NONBLOCK);
        }

        // Resolve hostname
        struct hostent* he = ::gethostbyname(_host.c_str());
        if (!he) {
            FN_LOGE(TAG, "Failed to resolve hostname: %s", _host.c_str());
            ::close(_socketFd);
            _socketFd = -1;
            return;
        }

        // Setup remote address
        std::memset(&_remoteAddr, 0, sizeof(_remoteAddr));
        _remoteAddr.sin_family = AF_INET;
        _remoteAddr.sin_port = htons(_port);
        std::memcpy(&_remoteAddr.sin_addr, he->h_addr_list[0], he->h_length);

        // Connect UDP socket (sets default destination)
        if (::connect(_socketFd, reinterpret_cast<struct sockaddr*>(&_remoteAddr), sizeof(_remoteAddr)) < 0) {
            FN_LOGE(TAG, "Failed to connect UDP socket: %s", std::strerror(errno));
            ::close(_socketFd);
            _socketFd = -1;
            return;
        }

        _connected = true;
        FN_LOGI(TAG, "Connected to %s:%u", _host.c_str(), static_cast<unsigned>(_port));
    }

    ~UdpChannel() override {
        if (_socketFd >= 0) {
            ::close(_socketFd);
        }
    }

    bool available() override {
        if (_socketFd < 0 || !_connected) {
            return false;
        }

        struct pollfd pfd;
        pfd.fd = _socketFd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = ::poll(&pfd, 1, 0);
        if (ret <= 0) {
            return false;
        }
        return (pfd.revents & POLLIN) != 0;
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
        if (_socketFd < 0 || !_connected) {
            return 0;
        }

        ssize_t n = ::recv(_socketFd, buffer, maxLen, 0);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                FN_LOGE(TAG, "Read error: %s", std::strerror(errno));
            }
            return 0;
        }
        
        // Debug: log received UDP packet (prefix only, to avoid spam)
        {
            char hex[3 * 16 + 1];
            format_hex_prefix(buffer, static_cast<std::size_t>(n), hex, sizeof(hex));
            if (n > 16) {
                FN_LOGD(TAG, "Received %zd bytes: %s ...", n, hex);
            } else {
                FN_LOGD(TAG, "Received %zd bytes: %s", n, hex);
            }
        }
        
        return static_cast<std::size_t>(n);
    }

    void write(const std::uint8_t* buffer, std::size_t len) override {
        if (_socketFd < 0 || !_connected) {
            return;
        }

        // Debug: log sent UDP packet (prefix only, to avoid spam)
        {
            char hex[3 * 16 + 1];
            format_hex_prefix(buffer, len, hex, sizeof(hex));
            if (len > 16) {
                FN_LOGD(TAG, "Sending %zu bytes: %s ...", len, hex);
            } else {
                FN_LOGD(TAG, "Sending %zu bytes: %s", len, hex);
            }
        }

        const std::uint8_t* ptr = buffer;
        std::size_t remaining = len;

        while (remaining > 0) {
            ssize_t n = ::send(_socketFd, ptr, remaining, 0);
            if (n <= 0) {
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    FN_LOGE(TAG, "Write error: %s", std::strerror(errno));
                }
                break;
            }
            remaining -= static_cast<std::size_t>(n);
            ptr += n;
        }
    }

private:
    std::string _host;
    std::uint16_t _port;
    int _socketFd;
    bool _connected;
    struct sockaddr_in _remoteAddr;
};

std::unique_ptr<fujinet::io::Channel> create_udp_channel(const std::string& host, std::uint16_t port)
{
    return std::make_unique<UdpChannel>(host, port);
}

} // namespace fujinet::platform

#endif // !_WIN32
