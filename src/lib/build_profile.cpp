#include "fujinet/build/profile.h"

namespace fujinet::build {

/*
    // Future definition for atari SIO on GPIO
    // FN_BUILD_ESP32_SIO
    return {
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::HardwareSio,
        .name             = "S3 + SIO via GPIO",
    };
    
    // FN_BUILD_LINUX_PI_USB
    return {
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .name             = "Pi + FujiBus over USB Gadget",
    };
*/

BuildProfile current_build_profile()
{
    BuildProfile profile{};

#if defined(FN_BUILD_ATARI)
    profile = BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::Pty,   // placeholder
        .name             = "Atari + SIO",
        .hw               = {},
    };
#elif defined(FN_BUILD_ESP32_USB_CDC)
    profile = BuildProfile{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .name             = "S3 + FujiBus over USB CDC",
        .hw               = {},
    };
#elif defined(FN_BUILD_FUJIBUS_PTY)
    profile = BuildProfile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "POSIX + FujiBus over PTY",
        .hw               = {},
    };
#else
    // Default: POSIX-friendly profile when no explicit build profile macro is provided.
    // This keeps local/test builds working without requiring a preset.
    profile = BuildProfile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "POSIX + FujiBus over PTY (default)",
        .hw               = {},
    };
#endif

    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
