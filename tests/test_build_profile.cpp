#include "doctest.h"

#include "fujinet/build/profile.h"

using namespace fujinet;

TEST_CASE("current build profile maps RS-232 preset to FujiBus over SerialPort")
{
#if defined(FN_BUILD_AMIGA_RS232)
    const auto profile = build::current_build_profile();
    CHECK(profile.machine == build::Machine::Generic);
    CHECK(profile.primaryTransport == build::TransportKind::FujiBusSlip);
    CHECK(profile.primaryChannel == build::ChannelKind::SerialPort);
    CHECK(profile.name == "POSIX + FujiBus over RS-232 (Amiga prototype)");
#else
    CHECK(true);
#endif
}

TEST_CASE("current build profile maps Atari packet-native NetSIO preset to direct SIO ingress")
{
#if defined(FN_BUILD_ATARI_FUJIBUS_NETSIO)
    const auto profile = build::current_build_profile();
    CHECK(profile.machine == build::Machine::Atari8Bit);
    CHECK(profile.primaryTransport == build::TransportKind::None);
    CHECK(profile.primaryChannel == build::ChannelKind::UdpSocket);
    CHECK(profile.name == "Atari packet-native NIO over NetSIO (POSIX)");
#else
    CHECK(true);
#endif
}

TEST_CASE("current build profile maps Zorro preset to FujiBusNative")
{
#if defined(FN_BUILD_ZORRO)
    const auto profile = build::current_build_profile();
    CHECK(profile.machine == build::Machine::Generic);
    CHECK(profile.primaryTransport == build::TransportKind::FujiBusNative);
    CHECK(profile.name == "Zorro + FujiBus over packet-native channel (stub)");
    CHECK_FALSE(profile.packetLink); // the PTY placeholder stays without packet I/O
#else
    CHECK(true);
#endif
}

TEST_CASE("current build profile maps the packet-link TCP preset to FujiBusNative over TCP")
{
#if defined(FN_BUILD_FUJIBUS_LINK_TCP)
    const auto profile = build::current_build_profile();
    CHECK(profile.machine == build::Machine::Generic);
    CHECK(profile.primaryTransport == build::TransportKind::FujiBusNative);
    CHECK(profile.primaryChannel == build::ChannelKind::TcpSocket);
    CHECK(profile.packetLink);
#else
    CHECK(true);
#endif
}

TEST_CASE("current build profile maps the packet-link serial preset to FujiBusNative over SerialPort")
{
#if defined(FN_BUILD_FUJIBUS_LINK_SERIAL)
    const auto profile = build::current_build_profile();
    CHECK(profile.machine == build::Machine::Generic);
    CHECK(profile.primaryTransport == build::TransportKind::FujiBusNative);
    CHECK(profile.primaryChannel == build::ChannelKind::SerialPort);
    CHECK(profile.packetLink);
#else
    CHECK(true);
#endif
}
