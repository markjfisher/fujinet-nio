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

#if defined(FN_BUILD_ATARI_SIO)
    profile = BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::HardwareSio,
        .name             = "Atari + SIO via GPIO",
        .hw               = {},
    };
#elif defined(FN_BUILD_ATARI_PTY)
    profile = BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "Atari + SIO over PTY (POSIX)",
        .hw               = {},
    };
#elif defined(FN_BUILD_ATARI_NETSIO)
    profile = BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::UdpSocket,
        .name             = "Atari + SIO over NetSIO (UDP)",
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
#elif defined(FN_BUILD_ESP32_FUJIBUS_GPIO)
    profile = BuildProfile{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::HardwareSio,
        .name             = "S3 + FujiBus over GPIO (e.g. RS232)",
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
