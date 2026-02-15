// Console transport using stdin/stdout (VFS). Used when CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG:
// logs go to USB Serial JTAG and we did not install UART0, so CLI must use the same (stdin/stdout).

#include "fujinet/console/console_engine.h"

#include <string>
#include <string_view>

extern "C" {
#include <unistd.h>
#include <fcntl.h>
}

namespace fujinet::console {

namespace {

class Esp32StdioConsoleTransport final : public IConsoleTransport {
public:
    Esp32StdioConsoleTransport()
    {
        const int fd = STDIN_FILENO;
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    bool read_byte(std::uint8_t& out, int timeout_ms) override
    {
        (void)timeout_ms;
        unsigned char byte;
        const ssize_t n = ::read(STDIN_FILENO, &byte, 1);
        if (n == 1) {
            out = byte;
            return true;
        }
        return false;
    }

    void write(std::string_view s) override
    {
        (void)::write(STDOUT_FILENO, s.data(), static_cast<size_t>(s.size()));
    }

    void write_line(std::string_view s) override
    {
        write(s);
        write("\r\n");
    }

private:
};

} // namespace

std::unique_ptr<IConsoleTransport> create_console_transport_stdio()
{
    return std::make_unique<Esp32StdioConsoleTransport>();
}

} // namespace fujinet::console
