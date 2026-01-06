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

    bool read_line(std::string& out, int timeout_ms) override
    {
        if (_masterFd < 0) {
            return false;
        }

        if (try_extract_line(out)) {
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
        for (;;) {
            ssize_t n = ::read(_masterFd, tmp, sizeof(tmp));
            if (n <= 0) {
                break;
            }
            _buf.append(tmp, static_cast<std::size_t>(n));

            // Safety bound: avoid unbounded growth if no newline arrives.
            if (_buf.size() > 1024) {
                _buf.clear();
                out.clear();
                return true;
            }
        }

        return try_extract_line(out);
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
    bool try_extract_line(std::string& out)
    {
        // Treat either CR or LF as end-of-line (screen often sends CR).
        const std::size_t eol = _buf.find_first_of("\r\n");
        if (eol == std::string::npos) {
            return false;
        }

        std::string line = _buf.substr(0, eol);

        // Consume EOL. If it's CRLF or LFCR, consume both.
        std::size_t consume = 1;
        if (eol + 1 < _buf.size()) {
            const char a = _buf[eol];
            const char b = _buf[eol + 1];
            if ((a == '\r' && b == '\n') || (a == '\n' && b == '\r')) {
                consume = 2;
            }
        }
        _buf.erase(0, eol + consume);

        out = std::move(line);
        return true;
    }

    int _masterFd{-1};
    std::string _buf;
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

    ::close(slaveFd);

    std::cout << "[Console] PTY created. Connect diagnostic console to: "
              << slaveName << "\n";

    return std::make_unique<PtyConsoleTransport>(masterFd);
}

} // namespace fujinet::console


