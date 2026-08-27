#include "doctest.h"

#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/platform/channel_factory.h"

#if defined(FN_PLATFORM_POSIX) && !defined(_WIN32)

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace fujinet;

namespace {

build::BuildProfile profile_for(build::ChannelKind channel,
                                build::TransportKind transport = build::TransportKind::FujiBus,
                                build::Machine machine = build::Machine::Generic)
{
    build::BuildProfile profile{};
    profile.machine = machine;
    profile.primaryTransport = transport;
    profile.primaryChannel = channel;
    return profile;
}

std::filesystem::path make_temp_dir()
{
    std::string templ = "/tmp/fujinet-nio-channel-test-XXXXXX";
    char* path = ::mkdtemp(templ.data());
    REQUIRE(path != nullptr);
    return path;
}

bool path_is_symlink(const std::filesystem::path& path)
{
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

std::uint16_t free_loopback_tcp_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    socklen_t addr_len = sizeof(addr);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) == 0);
    ::close(fd);
    return ntohs(addr.sin_port);
}

} // namespace

TEST_CASE("POSIX channel factory creates PTY channel and owns configured symlink")
{
    const auto tempDir = make_temp_dir();
    const auto ptyLink = tempDir / "fujinet-pty";

    config::FujiConfig cfg{};
    cfg.channel.ptyPath = ptyLink.string();

    auto channel = platform::create_channel_for_profile(profile_for(build::ChannelKind::Pty), cfg);
    REQUIRE(channel != nullptr);
    CHECK(path_is_symlink(ptyLink));
    CHECK(channel->supports_readable_wait());

    const int slaveFd = ::open(ptyLink.c_str(), O_WRONLY | O_NOCTTY | O_NONBLOCK);
    REQUIRE(slaveFd >= 0);
    const char byte = 'x';
    REQUIRE(::write(slaveFd, &byte, 1) == 1);
    ::close(slaveFd);
    CHECK(channel->wait_for_readable(std::chrono::milliseconds(100)));

    channel.reset();
    CHECK(!std::filesystem::exists(ptyLink));
    std::filesystem::remove_all(tempDir);
}

TEST_CASE("POSIX channel factory creates TCP server channel")
{
    config::FujiConfig cfg{};
    cfg.channel.tcpHost = "127.0.0.1";
    cfg.channel.tcpPort = 0;

    auto channel = platform::create_channel_for_profile(profile_for(build::ChannelKind::TcpSocket), cfg);
    REQUIRE(channel != nullptr);
    CHECK(!channel->available());
}

TEST_CASE("POSIX TCP channel retains readable bytes after peer half-close")
{
    config::FujiConfig cfg{};
    cfg.channel.tcpHost = "127.0.0.1";
    cfg.channel.tcpPort = free_loopback_tcp_port();

    auto channel = platform::create_channel_for_profile(profile_for(build::ChannelKind::TcpSocket), cfg);
    REQUIRE(channel != nullptr);

    const int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(client_fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(cfg.channel.tcpPort);
    REQUIRE(::connect(client_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    const char byte = 'x';
    REQUIRE(::write(client_fd, &byte, 1) == 1);
    REQUIRE(::shutdown(client_fd, SHUT_WR) == 0);

    CHECK(channel->available());
    std::uint8_t received = 0;
    CHECK(channel->read(&received, 1) == 1);
    CHECK(received == static_cast<std::uint8_t>(byte));
    ::close(client_fd);
}

TEST_CASE("POSIX channel factory creates generic UDP channel")
{
    config::FujiConfig cfg{};
    cfg.netsio.host = "127.0.0.1";
    cfg.netsio.port = 9;

    auto channel = platform::create_channel_for_profile(profile_for(build::ChannelKind::UdpSocket), cfg);
    REQUIRE(channel != nullptr);
    CHECK(!channel->available());
}

TEST_CASE("POSIX channel factory wraps Atari FujiBus UDP channel with NetSIO adapter")
{
    config::FujiConfig cfg{};
    cfg.netsio.host = "127.0.0.1";
    cfg.netsio.port = 9;

    auto channel = platform::create_channel_for_profile(
        profile_for(build::ChannelKind::UdpSocket,
                    build::TransportKind::FujiBus,
                    build::Machine::Atari8Bit),
        cfg);
    REQUIRE(channel != nullptr);
    CHECK(!channel->available());
}

TEST_CASE("POSIX channel factory rejects unsupported hardware-only channels")
{
    config::FujiConfig cfg{};

    CHECK(platform::create_channel_for_profile(profile_for(build::ChannelKind::UsbCdcDevice), cfg) == nullptr);
    CHECK(platform::create_channel_for_profile(profile_for(build::ChannelKind::UartGpio), cfg) == nullptr);
    CHECK(platform::create_channel_for_profile(profile_for(build::ChannelKind::SioGpio), cfg) == nullptr);
}

#endif
