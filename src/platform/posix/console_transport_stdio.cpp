#include "fujinet/console/console_engine.h"

#include <iostream>
#include <string>

#if !defined(_WIN32)
#include <poll.h>
#include <unistd.h>
#endif

namespace fujinet::console {

namespace {

class StdioConsoleTransport final : public IConsoleTransport {
public:
    bool read_line(std::string& out, int timeout_ms) override
    {
#if defined(_WIN32)
        (void)timeout_ms;
        if (!std::getline(std::cin, out)) {
            out = "exit";
        }
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
            out = "exit";
            return true;
        }

        if ((pfd.revents & (POLLHUP | POLLERR)) != 0) {
            out = "exit";
            return true;
        }

        if ((pfd.revents & POLLIN) == 0) {
            return false;
        }

        if (!std::getline(std::cin, out)) {
            out = "exit";
        }
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
};

} // namespace

// Internal POSIX factory used by console_transport_default.cpp
std::unique_ptr<IConsoleTransport> create_console_transport_stdio()
{
    return std::make_unique<StdioConsoleTransport>();
}

} // namespace fujinet::console