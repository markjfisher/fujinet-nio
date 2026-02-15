#pragma once

namespace fujinet::platform {

// Per-platform early init: console and log output backend for this board.
// On ESP32-S3 with TinyUSB: brings up USB and redirects esp_log to a CDC port.
// On standard ESP32 (no TinyUSB): leaves default UART console (CONFIG_ESP_CONSOLE_UART).
// Call first in app_main(); no logging before this returns.
void early_init();

} // namespace fujinet::platform
