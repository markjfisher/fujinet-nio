#include "fujinet/platform/posix/pty_channel.h"

#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <unistd.h>

#if defined(__linux__)
    #include <pty.h>
#elif defined(__APPLE__)
    #include <util.h>
#else
    #include <pty.h>
#endif

namespace fujinet::platform::posix {

class PtyChannel : public fujinet::io::Channel {
public:
    explicit PtyChannel(int masterFd, std::string symlinkPath = "")
        : _masterFd(masterFd)
        , _symlinkPath(std::move(symlinkPath))
    {}

    ~PtyChannel() override
    {
        if (_masterFd >= 0) {
            ::close(_masterFd);
        }
        if (!_symlinkPath.empty()) {
            ::unlink(_symlinkPath.c_str());
        }
    }

    bool available() override
    {
        if (_masterFd < 0) {
            return false;
        }

        pollfd pfd{};
        pfd.fd = _masterFd;
        pfd.events = POLLIN;

        const int ret = ::poll(&pfd, 1, 0);
        if (ret <= 0) {
            return false;
        }
        return (pfd.revents & POLLIN) != 0;
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        if (_masterFd < 0) {
            return 0;
        }

        const ssize_t n = ::read(_masterFd, buffer, maxLen);
        if (n <= 0) {
            return 0;
        }
        return static_cast<std::size_t>(n);
    }

    void write(const std::uint8_t* buffer, std::size_t len) override
    {
        if (_masterFd < 0) {
            return;
        }

        const std::uint8_t* ptr = buffer;
        std::size_t remaining = len;

        while (remaining > 0) {
            const ssize_t n = ::write(_masterFd, ptr, remaining);
            if (n <= 0) {
                break;
            }
            remaining -= static_cast<std::size_t>(n);
            ptr += n;
        }
    }

private:
    int _masterFd;
    std::string _symlinkPath;
};

std::unique_ptr<fujinet::io::Channel> create_pty_channel(const config::FujiConfig& config)
{
    int masterFd = -1;
    int slaveFd = -1;
    char slaveName[256] = {0};

    if (::openpty(&masterFd, &slaveFd, slaveName, nullptr, nullptr) != 0) {
        std::perror("openpty");
        return nullptr;
    }

    ::close(slaveFd);

    const int flags = ::fcntl(masterFd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
    }

    std::string symlinkPath;
    if (!config.channel.ptyPath.empty()) {
        ::unlink(config.channel.ptyPath.c_str());
        if (::symlink(slaveName, config.channel.ptyPath.c_str()) == 0) {
            symlinkPath = config.channel.ptyPath;
            std::cout << "[PtyChannel] Created symlink: " << symlinkPath << " -> " << slaveName << std::endl;
        } else {
            std::perror("symlink");
        }
    }

    std::cout << "[PtyChannel] Created PTY. Connect to slave: "
              << slaveName << std::endl;

    return std::make_unique<PtyChannel>(masterFd, std::move(symlinkPath));
}

} // namespace fujinet::platform::posix
