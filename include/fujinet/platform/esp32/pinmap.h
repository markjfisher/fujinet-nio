#pragma once

#include <cstdint>

namespace fujinet::platform::esp32 {

struct SdSpiPins {
    int mosi;
    int miso;
    int sck;
    int cs;
};

struct SioPins {
    int cmd;      // Command line (input)
    int int_pin;  // Interrupt line (output, open drain)
    int mtr;      // Motor line (input)
    int proc;     // Proceed line (output, open drain)
    int cki;      // Clock in (output, open drain)
    int cko;      // Clock out (input)
    int uart_rx;  // UART RX for SIO data
    int uart_tx;  // UART TX for SIO data
};

struct PinMap {
    SdSpiPins sd;
    SioPins sio;
};

/// Returns the globally selected pin mapping for this build.
const PinMap& pinmap();

} // namespace fujinet::platform::esp32
