#pragma once

#include <cstdint>

namespace fujinet::platform::esp32 {

struct SdSpiPins {
    int mosi = -1;
    int miso = -1;
    int sck = -1;
    int cs = -1;
};

struct UartPins {
    int rx = -1;
    int tx = -1;
};

struct SioPins {
    int cmd = -1;      // Command line (input)
    int int_pin = -1;  // Interrupt line (output, open drain)
    int mtr = -1;      // Motor line (input)
    int proc = -1;     // Proceed line (output, open drain)
    int cki = -1;      // Clock in (output, open drain)
    int cko = -1;      // Clock out (input)
    UartPins uart;     // UART for SIO data
};

struct Rs232Pins {
    UartPins uart;     // UART for RS232 data
    int ri = -1;          // Ring Indicator (output)
    int dcd = -1;         // Data Carrier Detect (output)
    int rts = -1;         // Request to Send (input)
    int cts = -1;         // Clear to Send (output)
    int dtr = -1;         // Data Terminal Ready (input)
    int dsr = -1;         // Data Set Ready (output)
    int invalid = -1;     // RS232 Invalid Data signal (input)
};

struct PinMap {
    SdSpiPins sd;
    SioPins sio;
    Rs232Pins rs232;
    
    /// Returns the primary UART pins for this board configuration.
    /// For RS232 boards, returns RS232 UART pins.
    /// For SIO boards, returns SIO UART pins.
    /// Returns {-1, -1} if no UART is configured.
    UartPins primaryUart() const;
};

/// Returns the globally selected pin mapping for this build.
const PinMap& pinmap();

} // namespace fujinet::platform::esp32
