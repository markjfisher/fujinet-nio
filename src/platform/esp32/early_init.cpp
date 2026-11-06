#include "fujinet/platform/early_init.h"
#include "fujinet/platform/esp32/tinyusb_cdc.h"

extern "C" {
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace fujinet::platform {

// Install log output for this board. When USB CDC is enabled, redirects esp_log
// to that port; otherwise leaves ESP-IDF default (e.g. UART0).
extern "C" void platform_install_log_output(void);

void early_init()
{
    platform_usb_console_early_init();

#if defined(FN_DEBUG_BOOT_COUNTDOWN) && CONFIG_TINYUSB_CDC_ENABLED
    // Give the PC time to reopen the enumerated console. Write directly to CDC
    // before installing the log redirect, independent of FN_DEBUG/log levels.
    const auto port = (CONFIG_FN_ESP_CONSOLE_CDC_NUM == 0)
        ? esp32::UsbCdcAcmPort::Port0 : esp32::UsbCdcAcmPort::Port1;
    vTaskDelay(pdMS_TO_TICKS(5000));
    constexpr char banner[] = "\r\nbooting up\r\n";
    esp32::write_cdc_port(port, banner, sizeof(banner) - 1);
    for (int remaining = 5; remaining > 0; --remaining) {
        const char line[] = {static_cast<char>('0' + remaining), '\r', '\n'};
        esp32::write_cdc_port(port, line, sizeof(line));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    constexpr char resume[] = "starting application\r\n";
    esp32::write_cdc_port(port, resume, sizeof(resume) - 1);
#endif

    platform_install_log_output();
}

} // namespace fujinet::platform
