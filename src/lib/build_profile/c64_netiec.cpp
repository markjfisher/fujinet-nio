#include "fujinet/build/profile.h"

namespace fujinet::build {

BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine          = Machine::C64,
        .primaryTransport = TransportKind::None,
        .primaryChannel   = ChannelKind::TcpSocket,
        .name             = "C64 NIO over NetIEC (POSIX)",
        .hw               = {},
        .defaultBootConfigUri = "persist:/boot/c64/autorun.d64",
        .defaultChannelTcpPort = 1541, // where VICE's NetIEC bridge looks
    };
    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
