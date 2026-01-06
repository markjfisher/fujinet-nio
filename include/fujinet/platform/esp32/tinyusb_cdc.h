#pragma once

#include <cstdint>

namespace fujinet::platform::esp32 {

// Shared TinyUSB initialization helpers for ESP32 USB-OTG CDC ACM.
// We centralize this so we don't attempt to install the TinyUSB driver/PHY twice
// from different translation units (which can fail with "PHY is in use").

enum class UsbCdcAcmPort : std::uint8_t {
    Port0 = 0,
    Port1 = 1,
};

// Install the TinyUSB driver (USB-OTG) if needed.
// Returns true if the driver is installed/ready.
bool ensure_tinyusb_driver();

// Ensure a CDC ACM port is initialized.
// Returns true if the port is initialized/ready.
bool ensure_tinyusb_cdc_acm(UsbCdcAcmPort port);

} // namespace fujinet::platform::esp32


