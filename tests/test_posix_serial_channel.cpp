#include "doctest.h"

#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/posix/serial_channel.h"

#if defined(FN_PLATFORM_POSIX) && !defined(_WIN32)

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

using namespace fujinet;

namespace {

class ScopedEnv {
public:
    explicit ScopedEnv(const char* name)
        : _name(name)
    {
        const char* value = std::getenv(name);
        if (value) {
            _hadValue = true;
            _value = value;
        }
    }

    ~ScopedEnv()
    {
        if (_hadValue) {
            ::setenv(_name.c_str(), _value.c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }

private:
    std::string _name;
    bool _hadValue{false};
    std::string _value;
};

} // namespace

TEST_CASE("POSIX serial helpers resolve config and baud fallback")
{
    config::FujiConfig cfg{};
    cfg.channel.serialPort = "/tmp/fujinet-test-serial";
    cfg.channel.uart.baudRate = 31250;

    ScopedEnv serialPort("FN_SERIAL_PORT");
    ScopedEnv serialBaud("FN_SERIAL_BAUD");
    ::unsetenv("FN_SERIAL_PORT");
    ::unsetenv("FN_SERIAL_BAUD");

    CHECK(!platform::posix::is_supported_serial_baud(31250));
    CHECK(platform::posix::effective_serial_baud(31250) == 19200u);

    const auto settings = platform::posix::resolve_serial_settings(cfg);
    CHECK(settings.port == "/tmp/fujinet-test-serial");
    CHECK(settings.uart.baudRate == 19200u);
    CHECK(settings.uart.dataBits == 8);
}

TEST_CASE("POSIX serial helpers allow environment overrides")
{
    config::FujiConfig cfg{};
    cfg.channel.serialPort = "/tmp/from-config";
    cfg.channel.uart.baudRate = 9600;
    cfg.channel.uart.dataBits = 7;
    cfg.channel.uart.parity = config::UartParity::Even;
    cfg.channel.uart.stopBits = config::UartStopBits::Two;

    ScopedEnv serialPort("FN_SERIAL_PORT");
    ScopedEnv serialBaud("FN_SERIAL_BAUD");
    ::setenv("FN_SERIAL_PORT", "/tmp/from-env", 1);
    ::setenv("FN_SERIAL_BAUD", "115200", 1);

    const auto settings = platform::posix::resolve_serial_settings(cfg);
    CHECK(settings.port == "/tmp/from-env");
    CHECK(settings.uart.baudRate == 115200u);
    CHECK(settings.uart.dataBits == 7);
    CHECK(settings.uart.parity == config::UartParity::Even);
    CHECK(settings.uart.stopBits == config::UartStopBits::Two);
}

TEST_CASE("POSIX serial channel can open a pseudo-terminal path")
{
    int masterFd = -1;
    int slaveFd = -1;
    char slaveName[256] = {0};
    REQUIRE(::openpty(&masterFd, &slaveFd, slaveName, nullptr, nullptr) == 0);
    ::close(slaveFd);

    config::UartConfig uart{};
    uart.baudRate = 9600;
    uart.dataBits = 8;

    auto channel = platform::posix::create_serial_channel_for_path(slaveName, uart);
    REQUIRE(channel != nullptr);

    const std::array<std::uint8_t, 3> incoming{{0x12, 0x34, 0x56}};
    REQUIRE(::write(masterFd, incoming.data(), incoming.size()) == static_cast<ssize_t>(incoming.size()));

    std::array<std::uint8_t, 8> buffer{};
    CHECK(channel->available());
    CHECK(channel->read(buffer.data(), buffer.size()) == incoming.size());
    CHECK(buffer[0] == 0x12);
    CHECK(buffer[1] == 0x34);
    CHECK(buffer[2] == 0x56);

    const std::array<std::uint8_t, 2> outgoing{{0xAB, 0xCD}};
    channel->write(outgoing.data(), outgoing.size());

    std::array<std::uint8_t, 8> masterBuffer{};
    REQUIRE(::read(masterFd, masterBuffer.data(), masterBuffer.size()) == static_cast<ssize_t>(outgoing.size()));
    CHECK(masterBuffer[0] == 0xAB);
    CHECK(masterBuffer[1] == 0xCD);

    ::close(masterFd);
}

TEST_CASE("POSIX serial channel applies UART framing settings")
{
    config::UartConfig uart{};
    uart.baudRate = 38400;
    uart.dataBits = 7;
    uart.parity = config::UartParity::Even;
    uart.stopBits = config::UartStopBits::Two;
    uart.flowControl = config::UartFlowControl::RtsCts;

    const struct termios tio = platform::posix::make_serial_termios(uart);

    CHECK((tio.c_cflag & CSIZE) == CS7);
    CHECK((tio.c_cflag & PARENB) != 0);
    CHECK((tio.c_cflag & PARODD) == 0);
    CHECK((tio.c_cflag & CSTOPB) != 0);
#if defined(CRTSCTS)
    CHECK((tio.c_cflag & CRTSCTS) != 0);
#endif
}

TEST_CASE("POSIX channel factory creates SerialPort channel from config path")
{
    int masterFd = -1;
    int slaveFd = -1;
    char slaveName[256] = {0};
    REQUIRE(::openpty(&masterFd, &slaveFd, slaveName, nullptr, nullptr) == 0);
    ::close(slaveFd);

    ScopedEnv serialPort("FN_SERIAL_PORT");
    ScopedEnv serialBaud("FN_SERIAL_BAUD");
    ::unsetenv("FN_SERIAL_PORT");
    ::unsetenv("FN_SERIAL_BAUD");

    config::FujiConfig cfg{};
    cfg.channel.serialPort = slaveName;
    cfg.channel.uart.baudRate = 9600;
    cfg.channel.uart.dataBits = 8;

    build::BuildProfile profile{};
    profile.primaryChannel = build::ChannelKind::SerialPort;
    profile.primaryTransport = build::TransportKind::FujiBus;

    auto channel = platform::create_channel_for_profile(profile, cfg);
    CHECK(channel != nullptr);

    ::close(masterFd);
}

#endif
