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
    explicit PtyChannel(int masterFd)
        : _masterFd(masterFd)
    {}

    ~PtyChannel() override {
        if (_masterFd >= 0) {
            ::close(_masterFd);
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
};

static std::unique_ptr<fujinet::io::Channel> create_pty_channel()
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

    std::cout << "[PtyChannel] Created PTY. Connect to slave: "
              << slaveName << std::endl;

    return std::make_unique<PtyChannel>(masterFd);
}

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const build::BuildProfile& profile, const config::FujiConfig& config)
{
    using build::ChannelKind;

    switch (profile.primaryChannel) {

    case ChannelKind::Pty:
        std::cout << "[ChannelFactory] Using PTY channel (Pty).\n";
        return create_pty_channel();

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

    case ChannelKind::HardwareSio:
        std::cout << "[ChannelFactory] HardwareSio not supported on POSIX (use Pty or UdpSocket for SIO testing).\n";
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
