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

TEST_CASE("current build profile maps Atari FujiBus-over-NetSIO preset to FujiBusSlip (not SIO)")
{
#if defined(FN_BUILD_ATARI_FUJIBUS_NETSIO)
    const auto profile = build::current_build_profile();
    CHECK(profile.machine == build::Machine::Atari8Bit);
    CHECK(profile.primaryTransport == build::TransportKind::FujiBusSlip);
    CHECK(profile.primaryChannel == build::ChannelKind::UdpSocket);
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
#else
    CHECK(true);
#endif
}
