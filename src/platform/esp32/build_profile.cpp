#include "fujinet/build/profile.h"

#include "sdkconfig.h"

#if defined(FN_BUILD_ESP32_USB_CDC) || defined(FN_BUILD_ESP32_USB_CDC_LINK)
static_assert(CONFIG_FN_FUJIBUS_USB_CDC_PORT != CONFIG_FN_ESP_CONSOLE_CDC_NUM,
              "FujiBus USB CDC and log output must use different interfaces");
static_assert(CONFIG_FN_FUJIBUS_USB_CDC_PORT < CONFIG_TINYUSB_CDC_COUNT &&
              CONFIG_FN_ESP_CONSOLE_CDC_NUM < CONFIG_TINYUSB_CDC_COUNT,
              "FujiBus USB CDC and log interfaces must both exist");
#endif

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
        .primaryTransport = TransportKind::None,
        .primaryChannel   = ChannelKind::SioGpio,
        .name             = "Atari packet-native NIO over SIO GPIO",
        .hw               = {},
        // The boot disk is flashed into the internal filesystem (boot_images = atari).
        // flash: names it explicitly; persist: would resolve to an SD card first.
        .defaultBootConfigUri = "flash:/boot/atari/autorun.atr",
    };
#elif defined(FN_BUILD_ESP32_USB_CDC)
    profile = BuildProfile{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .name             = "S3 + FujiBus over USB CDC",
        .hw               = {},
    };
#elif defined(FN_BUILD_ESP32_USB_CDC_LINK)
    profile = BuildProfile{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBusNative,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .packetLink       = true,
        .name             = "S3 + FujiBus over a packet link on USB CDC",
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
