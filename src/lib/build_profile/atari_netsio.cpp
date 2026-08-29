#include "fujinet/build/profile.h"

namespace fujinet::build {

BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::UdpSocket,
        .name             = "Atari + SIO over NetSIO (UDP)",
        .hw               = {},
    };
    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
