#include "fujinet/build/profile.h"

namespace fujinet::build {

// Default: POSIX-friendly profile when no explicit build profile macro is provided.
// This keeps local/test builds working without requiring a preset.
BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "POSIX + FujiBus over PTY (default)",
        .hw               = {},
    };
    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
