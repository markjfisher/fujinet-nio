#include "fujinet/build/profile.h"

namespace fujinet::build {

BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::SerialPort,
        .name             = "POSIX + FujiBus over RS-232 (Amiga prototype)",
        .hw               = {},
    };
    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
