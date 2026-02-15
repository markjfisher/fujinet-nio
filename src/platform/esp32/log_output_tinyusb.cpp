// Redirect esp_log output to TinyUSB CDC so that with a single USB cable we get
// logs on one CDC port and the diagnostic CLI on another. Do NOT use
// CONFIG_ESP_CONSOLE_USB_CDC (ROM stack) - it conflicts with TinyUSB.

#include "fujinet/platform/esp32/tinyusb_cdc.h"

#include <cstdarg>
#include <cstdio>

extern "C" {
#include "esp_log.h"
#include "sdkconfig.h"
}

#if CONFIG_TINYUSB_CDC_ENABLED

namespace {

constexpr std::size_t LOG_BUF_SIZE = 512;

static int tinyusb_log_vprintf(const char* fmt, va_list args)
{
    char buf[LOG_BUF_SIZE];
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0) {
        return n;
    }
    std::size_t len = static_cast<std::size_t>(n);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    const auto port = (CONFIG_FN_ESP_CONSOLE_CDC_NUM == 0)
        ? fujinet::platform::esp32::UsbCdcAcmPort::Port0
        : fujinet::platform::esp32::UsbCdcAcmPort::Port1;
    (void)fujinet::platform::esp32::write_cdc_port(port, buf, len);
    return n;
}

} // namespace

#endif // CONFIG_TINYUSB_CDC_ENABLED

extern "C" void platform_install_log_output(void)
{
#if CONFIG_TINYUSB_CDC_ENABLED
    esp_log_set_vprintf(&tinyusb_log_vprintf);
    // Banner so the user can identify which tty is the log port (Linux may show ACM1/2 not 0/1).
    const auto port = (CONFIG_FN_ESP_CONSOLE_CDC_NUM == 0)
        ? fujinet::platform::esp32::UsbCdcAcmPort::Port0
        : fujinet::platform::esp32::UsbCdcAcmPort::Port1;
    const char banner[] = "\r\n*** FujiNet logs (this port; build with FN_DEBUG for app logs) ***\r\n";
    fujinet::platform::esp32::write_cdc_port(port, banner, sizeof(banner) - 1);
    // One line through esp_log to confirm redirect works (tag nio so it's not filtered).
    ESP_LOGI("nio", "Log output redirected to this CDC port.");
#endif
}
