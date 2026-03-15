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
#include <poll.h>
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>

#if defined(__linux__)
    #include <pty.h>
#elif defined(__APPLE__)
    #include <util.h>
#else
    #include <pty.h>
#endif

namespace fujinet::platform {

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
        std::cout << "[ChannelFactory] TcpSocket channel not implemented yet.\n";
        return nullptr;

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
