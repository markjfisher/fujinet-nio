#include "fujinet/platform/channel_factory.h"

#include <memory>
#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>

#include "fujinet/io/core/channel.h"
#include "fujinet/build/profile.h"

#if !defined(_WIN32)

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace fujinet::platform {

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
            std::cerr << "[UdpChannel] Failed to create UDP socket: " 
                      << std::strerror(errno) << std::endl;
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
            std::cerr << "[UdpChannel] Failed to resolve hostname: " << _host << std::endl;
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
            std::cerr << "[UdpChannel] Failed to connect UDP socket: " 
                      << std::strerror(errno) << std::endl;
            ::close(_socketFd);
            _socketFd = -1;
            return;
        }

        _connected = true;
        std::cout << "[UdpChannel] Connected to " << _host << ":" << _port << std::endl;
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
                std::cerr << "[UdpChannel] Read error: " << std::strerror(errno) << std::endl;
            }
            return 0;
        }
        
        // Debug: log received UDP packet
        std::cout << "[UdpChannel] Received " << n << " bytes: ";
        for (ssize_t i = 0; i < n && i < 16; ++i) {
            std::printf("%02X ", buffer[i]);
        }
        if (n > 16) {
            std::cout << "...";
        }
        std::cout << std::endl;
        
        return static_cast<std::size_t>(n);
    }

    void write(const std::uint8_t* buffer, std::size_t len) override {
        if (_socketFd < 0 || !_connected) {
            return;
        }

        // Debug: log sent UDP packet
        std::cout << "[UdpChannel] Sending " << len << " bytes: ";
        for (std::size_t i = 0; i < len && i < 16; ++i) {
            std::printf("%02X ", buffer[i]);
        }
        if (len > 16) {
            std::cout << "...";
        }
        std::cout << std::endl;

        const std::uint8_t* ptr = buffer;
        std::size_t remaining = len;

        while (remaining > 0) {
            ssize_t n = ::send(_socketFd, ptr, remaining, 0);
            if (n <= 0) {
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::cerr << "[UdpChannel] Write error: " << std::strerror(errno) << std::endl;
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
