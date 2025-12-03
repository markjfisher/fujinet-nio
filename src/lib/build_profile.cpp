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
#if defined(FN_BUILD_ATARI)
    return BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::Pty,   // placeholder until we have real SIO HW
        .name             = "Atari + SIO",
    };
#elif defined(FN_BUILD_ESP32_USB_CDC)
    // This is your "Generic + FujiBus" build, which on ESP32 currently
    // uses TinyUSB CDC-ACM. So treat it as a USB CDC device channel.
    return BuildProfile{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .name             = "S3 + FujiBus over USB CDC",
    };
#else
    // Default POSIX dev build, etc. Uses PTY.
    return BuildProfile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "POSIX + FujiBus over PTY",
    };
#endif
}

} // namespace fujinet::build
