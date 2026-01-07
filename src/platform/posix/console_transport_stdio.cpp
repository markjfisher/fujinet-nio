#include "fujinet/console/console_engine.h"

#include <iostream>
#include <string>

#if !defined(_WIN32)
#include <termios.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace fujinet::console {

namespace {

class StdioConsoleTransport final : public IConsoleTransport {
public:
    StdioConsoleTransport()
    {
#if !defined(_WIN32)
        if (::isatty(STDIN_FILENO)) {
            if (::tcgetattr(STDIN_FILENO, &_orig) == 0) {
                termios t = _orig;
                // Minimal raw-ish mode for interactive editing.
                t.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
                t.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
                t.c_oflag |= OPOST;
                t.c_cc[VMIN] = 0;
                t.c_cc[VTIME] = 0;
                if (::tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
                    _hasTermios = true;
                }
            }
        }
#endif
    }

    ~StdioConsoleTransport() override
    {
#if !defined(_WIN32)
        if (_hasTermios) {
            (void)::tcsetattr(STDIN_FILENO, TCSANOW, &_orig);
        }
#endif
    }

    bool read_byte(std::uint8_t& out, int timeout_ms) override
    {
#if defined(_WIN32)
        (void)timeout_ms;
        const int ch = std::cin.get();
        if (ch == EOF) {
            return false;
        }
        out = static_cast<std::uint8_t>(ch);
        return true;
#else
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        pfd.revents = 0;

        int ret = ::poll(&pfd, 1, timeout_ms);
        if (ret == 0) {
            return false; // timeout
        }

        if (ret < 0) {
            return false;
        }

        if ((pfd.revents & (POLLHUP | POLLERR)) != 0) {
            return false;
        }

        if ((pfd.revents & POLLIN) == 0) {
            return false;
        }

        unsigned char ch = 0;
        const ssize_t n = ::read(STDIN_FILENO, &ch, 1);
        if (n != 1) {
            return false;
        }
        out = static_cast<std::uint8_t>(ch);
        return true;
#endif
    }

    void write(std::string_view s) override
    {
        std::cout.write(s.data(), static_cast<std::streamsize>(s.size()));
        std::cout.flush();
    }

    void write_line(std::string_view s) override
    {
        std::cout.write(s.data(), static_cast<std::streamsize>(s.size()));
        std::cout.put('\n');
        std::cout.flush();
    }

private:
#if !defined(_WIN32)
    bool _hasTermios{false};
    termios _orig{};
#endif
};

} // namespace

// Internal POSIX factory used by console_transport_default.cpp
std::unique_ptr<IConsoleTransport> create_console_transport_stdio()
{
    return std::make_unique<StdioConsoleTransport>();
}

} // namespace fujinet::console