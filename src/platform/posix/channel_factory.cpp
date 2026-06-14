#include "fujinet/platform/channel_factory.h"

#include <memory>
#include <iostream>
#include <cstdlib>
#include <string>

#include "fujinet/io/core/channel.h"
#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"

#if !defined(_WIN32)

// POSIX / Unix PTY implementation
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

#if defined(__linux__)
    #include <pty.h>
#elif defined(__APPLE__)
    #include <util.h>
#else
    #include <pty.h>
#endif

namespace fujinet::platform {

// ---------------------------------------------------------------------------
// SerialChannel — wraps a POSIX RS-232 serial port (8N1, raw, non-blocking)
// ---------------------------------------------------------------------------

class SerialChannel : public fujinet::io::Channel {
public:
    explicit SerialChannel(int fd) : _fd(fd) {}

    ~SerialChannel() override {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    bool available() override {
        if (_fd < 0) return false;
        struct pollfd pfd;
        pfd.fd      = _fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        return ::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0;
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
        if (_fd < 0) return 0;
        ssize_t n = ::read(_fd, buffer, maxLen);
        return (n > 0) ? static_cast<std::size_t>(n) : 0;
    }

    void write(const std::uint8_t* buffer, std::size_t len) override {
        if (_fd < 0) return;
        const std::uint8_t* ptr = buffer;
        std::size_t remaining = len;
        while (remaining > 0) {
            ssize_t n = ::write(_fd, ptr, remaining);
            if (n <= 0) break;
            ptr       += n;
            remaining -= static_cast<std::size_t>(n);
        }
    }

private:
    int _fd;
};

static speed_t baud_constant(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:     return B19200;
    }
}

static std::unique_ptr<fujinet::io::Channel> create_serial_channel(const config::FujiConfig& /*config*/)
{
    const char* port = std::getenv("FN_SERIAL_PORT");
    if (!port || port[0] == '\0') port = "/dev/ttyUSB0";

    const char* baud_str = std::getenv("FN_SERIAL_BAUD");
    int baud = (baud_str && baud_str[0] != '\0') ? std::atoi(baud_str) : 19200;

    int fd = ::open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::perror("[SerialChannel] open");
        return nullptr;
    }

    struct termios tio{};
    tio.c_cflag = CS8 | CLOCAL | CREAD;
    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_lflag = 0;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 1;
    ::cfsetispeed(&tio, baud_constant(baud));
    ::cfsetospeed(&tio, baud_constant(baud));

    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        std::perror("[SerialChannel] tcsetattr");
        ::close(fd);
        return nullptr;
    }
    ::tcflush(fd, TCIOFLUSH);

    std::cout << "[SerialChannel] Opened " << port << " at " << baud << " baud.\n";
    return std::make_unique<SerialChannel>(fd);
}

// ---------------------------------------------------------------------------

class PtyChannel : public fujinet::io::Channel {
public:
    explicit PtyChannel(int masterFd, std::string symlinkPath = "")
        : _masterFd(masterFd)
        , _symlinkPath(std::move(symlinkPath))
    {}

    ~PtyChannel() override {
        if (_masterFd >= 0) {
            ::close(_masterFd);
        }
        if (!_symlinkPath.empty()) {
            ::unlink(_symlinkPath.c_str());
        }
    }

    bool available() override {
        if (_masterFd < 0) {
            return false;
        }

        struct pollfd pfd;
        pfd.fd     = _masterFd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = ::poll(&pfd, 1, 0);
        if (ret <= 0) {
            return false;
        }
        return (pfd.revents & POLLIN) != 0;
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
        if (_masterFd < 0) {
            return 0;
        }

        ssize_t n = ::read(_masterFd, buffer, maxLen);
        if (n <= 0) {
            return 0;
        }
        return static_cast<std::size_t>(n);
    }

    void write(const std::uint8_t* buffer, std::size_t len) override {
        if (_masterFd < 0) {
            return;
        }

        const std::uint8_t* ptr = buffer;
        std::size_t remaining = len;

        while (remaining > 0) {
            ssize_t n = ::write(_masterFd, ptr, remaining);
            if (n <= 0) {
                break;
            }
            remaining -= static_cast<std::size_t>(n);
            ptr       += n;
        }
    }

private:
    int _masterFd;
    std::string _symlinkPath;
};

class TcpServerChannel : public fujinet::io::Channel {
public:
    TcpServerChannel(std::string host, std::uint16_t port)
        : _host(std::move(host))
        , _port(port)
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
        if (pfd.revents & (POLLHUP | POLLERR)) {
            close_client();
            return false;
        }
        return (pfd.revents & POLLIN) != 0;
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
        std::cout << "[TcpServerChannel] Client connected" << std::endl;
    }

    void close_client()
    {
        if (_clientFd >= 0) {
            ::close(_clientFd);
            _clientFd = -1;
            std::cout << "[TcpServerChannel] Client disconnected" << std::endl;
        }
    }

    std::string _host;
    std::uint16_t _port;
    int _listenFd{-1};
    int _clientFd{-1};
};

static std::unique_ptr<fujinet::io::Channel> create_pty_channel(const config::FujiConfig& config)
{
    int masterFd = -1;
    int slaveFd  = -1;
    char slaveName[256] = {0};

    if (::openpty(&masterFd, &slaveFd, slaveName, nullptr, nullptr) != 0) {
        std::perror("openpty");
        return nullptr;
    }

    ::close(slaveFd);

    int flags = ::fcntl(masterFd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
    }

    std::string symlinkPath;
    if (!config.channel.ptyPath.empty()) {
        // Remove any existing symlink or file at the path
        ::unlink(config.channel.ptyPath.c_str());
        if (::symlink(slaveName, config.channel.ptyPath.c_str()) == 0) {
            symlinkPath = config.channel.ptyPath;
            std::cout << "[PtyChannel] Created symlink: " << symlinkPath << " -> " << slaveName << std::endl;
        } else {
            std::perror("symlink");
            // Continue without symlink
        }
    }

    std::cout << "[PtyChannel] Created PTY. Connect to slave: "
              << slaveName << std::endl;

    return std::make_unique<PtyChannel>(masterFd, std::move(symlinkPath));
}

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const build::BuildProfile& profile, const config::FujiConfig& config)
{
    using build::ChannelKind;

    switch (profile.primaryChannel) {

     case ChannelKind::Pty:
         std::cout << "[ChannelFactory] Using PTY channel (Pty).\n";
         return create_pty_channel(config);

    case ChannelKind::UsbCdcDevice:
        std::cout << "[ChannelFactory] UsbCdcDevice not supported on POSIX.\n";
        return nullptr;

    case ChannelKind::TcpSocket:
        std::cout << "[ChannelFactory] Using TCP server channel (TcpSocket) on "
                  << config.channel.tcpHost << ":" << config.channel.tcpPort << std::endl;
        return std::make_unique<TcpServerChannel>(config.channel.tcpHost, config.channel.tcpPort);

    case ChannelKind::UdpSocket: {
        // Use NetSIO config from fujinet.yaml
        std::string host = config.netsio.host;
        std::uint16_t port = config.netsio.port;
        
        std::cout << "[ChannelFactory] Using UDP channel (NetSIO) to " << host << ":" << port << std::endl;
        
        // Forward declaration - implementation in udp_channel.cpp
        extern std::unique_ptr<fujinet::io::Channel> create_udp_channel(const std::string& host, std::uint16_t port);
        return create_udp_channel(host, port);
    }

    case ChannelKind::UartGpio:
        std::cout << "[ChannelFactory] UartGpio not supported on POSIX (use Pty or UdpSocket for SIO testing).\n";
        return nullptr;

    case ChannelKind::SerialPort:
        std::cout << "[ChannelFactory] Using RS-232 serial channel.\n";
        return create_serial_channel(config);
    }

    std::cout << "[ChannelFactory] Unknown ChannelKind.\n";
    return nullptr;
}

} // namespace fujinet::platform

#else // _WIN32

namespace fujinet::platform {

class DummyChannel : public fujinet::io::Channel {
public:
    bool available() override { return false; }
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
        (void)buffer; (void)maxLen; return 0;
    }
    void write(const std::uint8_t* buffer, std::size_t len) override {
        (void)buffer; (void)len;
    }
};

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const build::BuildProfile& /*profile*/, const config::FujiConfig& /*config*/)
{
    std::cout << "[PtyChannel] PTY not supported on this platform; "
                 "using dummy Channel.\n";
    return std::make_unique<DummyChannel>();
}

} // namespace fujinet::platform

#endif // !_WIN32
