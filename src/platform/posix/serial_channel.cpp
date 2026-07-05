#include "fujinet/platform/posix/serial_channel.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <unistd.h>

namespace fujinet::platform {

class SerialChannel : public fujinet::io::Channel {
public:
    explicit SerialChannel(int fd) : _fd(fd) {}

    ~SerialChannel() override
    {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    bool available() override
    {
        if (_fd < 0) {
            return false;
        }
        pollfd pfd{};
        pfd.fd = _fd;
        pfd.events = POLLIN;
        return ::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0;
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        if (_fd < 0) {
            return 0;
        }
        const ssize_t n = ::read(_fd, buffer, maxLen);
        return (n > 0) ? static_cast<std::size_t>(n) : 0;
    }

    void write(const std::uint8_t* buffer, std::size_t len) override
    {
        if (_fd < 0) {
            return;
        }
        const std::uint8_t* ptr = buffer;
        std::size_t remaining = len;
        while (remaining > 0) {
            const ssize_t n = ::write(_fd, ptr, remaining);
            if (n > 0) {
                ptr += n;
                remaining -= static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd pfd{};
                pfd.fd = _fd;
                pfd.events = POLLOUT;
                if (::poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLOUT) != 0) {
                    continue;
                }
            }
            std::perror("[SerialChannel] write");
            break;
        }
    }

private:
    int _fd;
};

} // namespace fujinet::platform

namespace fujinet::platform::posix {

bool is_supported_serial_baud(std::uint32_t baud)
{
    switch (baud) {
        case 9600:
        case 19200:
        case 38400:
        case 57600:
        case 115200:
        case 230400:
            return true;
        default:
            return false;
    }
}

std::uint32_t effective_serial_baud(std::uint32_t requested)
{
    return is_supported_serial_baud(requested) ? requested : 19200u;
}

static speed_t baud_constant(std::uint32_t baud)
{
    switch (effective_serial_baud(baud)) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:     return B19200;
    }
}

static int normalized_data_bits(int dataBits)
{
    return (dataBits >= 5 && dataBits <= 8) ? dataBits : 8;
}

static tcflag_t data_bits_flag(int dataBits)
{
    switch (normalized_data_bits(dataBits)) {
        case 5: return CS5;
        case 6: return CS6;
        case 7: return CS7;
        case 8:
        default: return CS8;
    }
}

termios make_serial_termios(const config::UartConfig& uart)
{
    termios tio{};
    tio.c_cflag = CLOCAL | CREAD | data_bits_flag(uart.dataBits);
    switch (uart.parity) {
        case config::UartParity::Even:
            tio.c_cflag |= PARENB;
            tio.c_cflag &= ~PARODD;
            break;
        case config::UartParity::Odd:
            tio.c_cflag |= PARENB;
            tio.c_cflag |= PARODD;
            break;
        case config::UartParity::None:
        default:
            tio.c_cflag &= ~PARENB;
            tio.c_cflag &= ~PARODD;
            break;
    }
    if (uart.stopBits == config::UartStopBits::Two ||
        uart.stopBits == config::UartStopBits::OnePointFive) {
        tio.c_cflag |= CSTOPB;
    } else {
        tio.c_cflag &= ~CSTOPB;
    }
#if defined(CRTSCTS)
    if (uart.flowControl == config::UartFlowControl::RtsCts) {
        tio.c_cflag |= CRTSCTS;
    } else {
        tio.c_cflag &= ~CRTSCTS;
    }
#endif
    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_lflag = 0;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;
    return tio;
}

SerialSettings resolve_serial_settings(const config::FujiConfig& config)
{
    SerialSettings settings{
        .port = config.channel.serialPort.empty() ? std::string{"/dev/ttyUSB0"} : config.channel.serialPort,
        .uart = config.channel.uart,
    };

    settings.uart.baudRate = effective_serial_baud(settings.uart.baudRate);
    settings.uart.dataBits = normalized_data_bits(settings.uart.dataBits);

    const char* envPort = std::getenv("FN_SERIAL_PORT");
    if (envPort && envPort[0] != '\0') {
        settings.port = envPort;
    }

    const char* envBaud = std::getenv("FN_SERIAL_BAUD");
    if (envBaud && envBaud[0] != '\0') {
        char* end = nullptr;
        const auto parsed = std::strtoul(envBaud, &end, 10);
        if (end && *end == '\0') {
            settings.uart.baudRate = effective_serial_baud(static_cast<std::uint32_t>(parsed));
        }
    }

    return settings;
}

std::unique_ptr<fujinet::io::Channel>
create_serial_channel_for_path(const std::string& port, const config::UartConfig& uart)
{
    const std::uint32_t effectiveBaud = effective_serial_baud(uart.baudRate);
    const int dataBits = normalized_data_bits(uart.dataBits);

    if (effectiveBaud != uart.baudRate) {
        std::cout << "[SerialChannel] Unsupported baud " << uart.baudRate
                  << "; using " << effectiveBaud << " instead.\n";
    }
    if (dataBits != uart.dataBits) {
        std::cout << "[SerialChannel] Unsupported data_bits " << uart.dataBits
                  << "; using " << dataBits << " instead.\n";
    }

    if (port.empty()) {
        std::cout << "[SerialChannel] Empty serial port path.\n";
        return nullptr;
    }

    const int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::perror("[SerialChannel] open");
        return nullptr;
    }

#if !defined(CRTSCTS)
    if (uart.flowControl == config::UartFlowControl::RtsCts) {
        std::cout << "[SerialChannel] RTS/CTS flow control not supported by this termios platform.\n";
    }
#endif
    termios tio = make_serial_termios(uart);
    ::cfsetispeed(&tio, baud_constant(effectiveBaud));
    ::cfsetospeed(&tio, baud_constant(effectiveBaud));

    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        std::perror("[SerialChannel] tcsetattr");
        ::close(fd);
        return nullptr;
    }
    ::tcflush(fd, TCIOFLUSH);

    std::cout << "[SerialChannel] Opened " << port
              << " at " << effectiveBaud
              << " baud, " << dataBits
              << " data bits.\n";
    return std::make_unique<fujinet::platform::SerialChannel>(fd);
}

std::unique_ptr<fujinet::io::Channel> create_serial_channel(const config::FujiConfig& config)
{
    const auto settings = resolve_serial_settings(config);
    return create_serial_channel_for_path(settings.port, settings.uart);
}

} // namespace fujinet::platform::posix
