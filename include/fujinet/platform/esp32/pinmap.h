#pragma once

#include <cstdint>

namespace fujinet::platform::esp32 {

struct SdSpiPins {
    int mosi;
    int miso;
    int sck;
    int cs;
};

struct PinMap {
    SdSpiPins sd;
    // Future: uart, leds, buttons, etc.
    // struct UartPins { int tx; int rx; };
    // UartPins sio;
};

/// Returns the globally selected pin mapping for this build.
const PinMap& pinmap();

} // namespace fujinet::platform::esp32
