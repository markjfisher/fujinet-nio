#include "fujinet/platform/early_init.h"
#include "fujinet/platform/esp32/tinyusb_cdc.h"

namespace fujinet::platform {

// Install log output for this board. When USB CDC is enabled, redirects esp_log
// to that port; otherwise leaves ESP-IDF default (e.g. UART0).
extern "C" void platform_install_log_output(void);

void early_init()
{
    platform_usb_console_early_init();
    platform_install_log_output();
}

} // namespace fujinet::platform
