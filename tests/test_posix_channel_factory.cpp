#include "doctest.h"

#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/platform/channel_factory.h"

#if defined(FN_PLATFORM_POSIX) && !defined(_WIN32)

#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
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
