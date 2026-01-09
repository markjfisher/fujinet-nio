#include "fujinet/console/console_engine.h"

#if defined(_WIN32)
#error "POSIX PTY console transport is not supported on Windows. Use the stdio console transport instead."
#endif

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
    #include <pty.h>
#elif defined(__APPLE__)
    #include <util.h>
#else
    #include <pty.h>
#endif

namespace fujinet::console {

namespace {

class PtyConsoleTransport final : public IConsoleTransport {
public:
    explicit PtyConsoleTransport(int masterFd)
        : _masterFd(masterFd)
    {
        if (_masterFd >= 0) {
            int flags = ::fcntl(_masterFd, F_GETFL, 0);
            if (flags >= 0) {
                ::fcntl(_masterFd, F_SETFL, flags | O_NONBLOCK);
            }
        }
    }

    ~PtyConsoleTransport() override
    {
        if (_masterFd >= 0) {
            ::close(_masterFd);
            _masterFd = -1;
        }
    }

    bool is_connected() const override
    {
        if (_masterFd < 0) return false;

        struct pollfd pfd;
        pfd.fd = _masterFd;
        pfd.events = POLLIN | POLLHUP | POLLERR | POLLNVAL;
        pfd.revents = 0;

        const int ret = ::poll(&pfd, 1, 0);
        if (ret < 0) {
            return false;
        }
        // When no slave is connected, PTY master commonly reports HUP/NVAL.
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            return false;
        }
        return true;
    }

    bool read_byte(std::uint8_t& out, int timeout_ms) override
    {
        if (_masterFd < 0) {
            return false;
        }

        if (_rx_off < _rx.size()) {
            out = static_cast<std::uint8_t>(_rx[_rx_off++]);
            if (_rx_off >= _rx.size()) {
                _rx.clear();
                _rx_off = 0;
            }
            return true;
        }

        struct pollfd pfd;
        pfd.fd = _masterFd;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        pfd.revents = 0;

        int ret = ::poll(&pfd, 1, timeout_ms);
        if (ret == 0) {
            return false; // timeout
        }

        if (ret < 0) {
            // Treat poll errors as "no input" so the core loop keeps running.
            return false;
        }

        // When no slave is connected yet, the PTY master may report HUP.
        // That is not a shutdown condition for the app; just report "no input".
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            return false;
        }

        if ((pfd.revents & POLLIN) == 0) {
            return false;
        }

        char tmp[128];
        ssize_t n = ::read(_masterFd, tmp, sizeof(tmp));
        if (n <= 0) {
            return false;
        }

        _rx.assign(tmp, static_cast<std::size_t>(n));
        _rx_off = 0;
        out = static_cast<std::uint8_t>(_rx[_rx_off++]);
        if (_rx_off >= _rx.size()) {
            _rx.clear();
            _rx_off = 0;
        }
        return true;
    }

    void write(std::string_view s) override
    {
        if (_masterFd < 0) {
            return;
        }

        const char* p = s.data();
        std::size_t remaining = s.size();
        while (remaining > 0) {
            ssize_t n = ::write(_masterFd, p, remaining);
            if (n <= 0) {
                break;
            }
            p += n;
            remaining -= static_cast<std::size_t>(n);
        }
    }

    void write_line(std::string_view s) override
    {
        write(s);
        write("\r\n");
    }

private:
    int _masterFd{-1};
    std::string _rx;
    std::size_t _rx_off{0};
};

} // namespace

// Internal POSIX factory used by console_transport_default.cpp
std::unique_ptr<IConsoleTransport> create_console_transport_pty()
{
    int masterFd = -1;
    int slaveFd = -1;
    char slaveName[256] = {0};

    if (::openpty(&masterFd, &slaveFd, slaveName, nullptr, nullptr) != 0) {
        std::perror("openpty(console)");
        return nullptr;
    }

    // Configure the slave side to avoid kernel line editing/echo.
    // The ConsoleEngine does its own line editing and echo; we want raw bytes.
    {
        termios tio{};
        if (::tcgetattr(slaveFd, &tio) == 0) {
            ::cfmakeraw(&tio);
            // Be explicit: no echo, no canonical mode.
            tio.c_lflag &= static_cast<unsigned>(~(ECHO | ECHONL | ICANON | ISIG | IEXTEN));
            tio.c_iflag &= static_cast<unsigned>(~(IXON | IXOFF | ICRNL | INLCR | IGNCR));
            tio.c_oflag &= static_cast<unsigned>(~(OPOST));
            (void)::tcsetattr(slaveFd, TCSANOW, &tio);
        }
        // Drop any pending input (paranoia: avoids stray bytes on first connect).
        (void)::tcflush(slaveFd, TCIFLUSH);
    }

    ::close(slaveFd);

    std::cout << "[Console] PTY created. Connect diagnostic console to: "
              << slaveName << "\n";

    return std::make_unique<PtyConsoleTransport>(masterFd);
}

} // namespace fujinet::console


