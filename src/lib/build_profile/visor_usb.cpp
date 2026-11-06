#include "fujinet/build/profile.h"
namespace fujinet::build {
BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine = Machine::Generic,
        .primaryTransport = TransportKind::FujiBusNative,
        .primaryChannel = ChannelKind::VisorUsbHost,
        .name = "POSIX + Handspring Visor USB Host (raw FujiBus)",
        .hw = {},
    };
    profile.hw = detect_hardware_capabilities();
    profile.hw.hasUsbHost = true;
    return profile;
}
} // namespace fujinet::build
