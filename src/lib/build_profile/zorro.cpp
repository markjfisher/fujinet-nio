#include "fujinet/build/profile.h"

namespace fujinet::build {

BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBusNative,
        .primaryChannel   = ChannelKind::Pty, // placeholder until Zorro has a real ChannelKind
        .name             = "Zorro + FujiBus over packet-native channel (stub)",
        .hw               = {},
    };
    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
