#include "fujinet/build/profile.h"

namespace fujinet::build {

BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::TcpSocket,
        .name             = "POSIX + FujiBus over TCP serial",
        .hw               = {},
    };
    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
