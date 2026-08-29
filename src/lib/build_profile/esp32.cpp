#include "fujinet/build/profile.h"

// esp32.cpp — consolidated ESP32 build profiles.
// IDF/PlatformIO compiles all lib/ sources together, so #ifdef selection is
// required here. This is the only build_profile/*.cpp file that may contain
// preprocessor conditionals.

namespace fujinet::build {

BuildProfile current_build_profile()
{
    BuildProfile profile{};

#if defined(FN_BUILD_ATARI_SIO)
    profile = BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::UartGpio,
        .name             = "Atari + SIO via GPIO",
        .hw               = {},
    };
#elif defined(FN_BUILD_ATARI_FUJIBUS_SIO)
    profile = BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::SioGpio,
        .name             = "Atari + FujiBus over SIO GPIO",
        .hw               = {},
    };
#elif defined(FN_BUILD_ESP32_USB_CDC)
    profile = BuildProfile{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .name             = "S3 + FujiBus over USB CDC",
        .hw               = {},
    };
#elif defined(FN_BUILD_ESP32_FUJIBUS_GPIO)
    profile = BuildProfile{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::UartGpio,
        .name             = "S3 + FujiBus over GPIO (e.g. RS232)",
        .hw               = {},
    };
#else
    // Default ESP32 fallback
    profile = BuildProfile{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .name             = "S3 + FujiBus over USB CDC (default)",
        .hw               = {},
    };
#endif

    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
