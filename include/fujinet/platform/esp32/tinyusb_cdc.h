#pragma once

#include <cstddef>
#include <cstdint>

// Call before any logging or task start. Installs TinyUSB driver and the
// CDC port used for logs (CONFIG_FN_ESP_CONSOLE_CDC_NUM). No logging inside.
#ifdef __cplusplus
extern "C" {
#endif
void platform_usb_console_early_init(void);
#ifdef __cplusplus
}
#endif

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

// Write bytes to a CDC port (e.g. for esp_log redirect). Non-blocking, best-effort.
// Safe to call from esp_log vprintf callback. Returns bytes written.
std::size_t write_cdc_port(UsbCdcAcmPort port, const char* buf, std::size_t len);

} // namespace fujinet::platform::esp32


