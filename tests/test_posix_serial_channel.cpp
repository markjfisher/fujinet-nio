#include "doctest.h"

#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_link.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/posix/serial_channel.h"

#if defined(FN_PLATFORM_POSIX) && !defined(_WIN32)

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

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
    profile.primaryTransport = build::TransportKind::FujiBusSlip;

    auto channel = platform::create_channel_for_profile(profile, cfg);
    CHECK(channel != nullptr);

    ::close(masterFd);
}

TEST_CASE("POSIX channel factory carries a SerialPort over a packet link when the profile asks")
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

    build::BuildProfile profile{};
    profile.primaryChannel = build::ChannelKind::SerialPort;
    profile.primaryTransport = build::TransportKind::FujiBusNative;
    profile.packetLink = true;

    auto channel = platform::create_channel_for_profile(profile, cfg);
    REQUIRE(channel != nullptr);
    REQUIRE(dynamic_cast<io::PacketLinkChannel*>(channel.get()) != nullptr);
    auto* packets = channel->packet_io();
    REQUIRE(packets != nullptr);

    // A controller on the other end of the line syncs and gets its SyncAck.
    const std::uint8_t version = io::PacketLink::kVersion;
    const auto sync = io::PacketLink::encode(io::PacketLink::Kind::Sync, 9, &version, 1);
    REQUIRE(::write(masterFd, sync.data(), sync.size()) == static_cast<ssize_t>(sync.size()));

    // version 1, the default capacity (4096, little-endian), the default record timeout
    const std::uint8_t body[] = {1, 0x00, 0x10, 100, 0};
    const auto expected = io::PacketLink::encode(io::PacketLink::Kind::SyncAck, 9, body, sizeof(body));

    std::vector<std::uint8_t> reply;
    std::uint8_t buffer[io::PacketLink::kDefaultCapacity];
    for (int i = 0; i < 100 && reply.size() < expected.size(); ++i) {
        (void)packets->receive(buffer, sizeof(buffer));
        pollfd pfd{};
        pfd.fd = masterFd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 20) > 0) {
            std::uint8_t chunk[64];
            const ssize_t n = ::read(masterFd, chunk, sizeof(chunk));
            if (n > 0) reply.insert(reply.end(), chunk, chunk + n);
        }
    }

    CHECK(reply == expected);

    ::close(masterFd);
}

TEST_CASE("POSIX channel factory leaves a SerialPort bare when the profile has no packet link")
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

    build::BuildProfile profile{};
    profile.primaryChannel = build::ChannelKind::SerialPort;
    profile.primaryTransport = build::TransportKind::FujiBusSlip;

    auto channel = platform::create_channel_for_profile(profile, cfg);
    REQUIRE(channel != nullptr);
    CHECK(dynamic_cast<io::PacketLinkChannel*>(channel.get()) == nullptr);

    ::close(masterFd);
}

#endif
