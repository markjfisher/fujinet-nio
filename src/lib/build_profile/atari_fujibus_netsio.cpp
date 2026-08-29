#include "fujinet/build/profile.h"

namespace fujinet::build {

BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::UdpSocket,
        .name             = "Atari + FujiBus over NetSIO (POSIX)",
        .hw               = {},
    };
    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
