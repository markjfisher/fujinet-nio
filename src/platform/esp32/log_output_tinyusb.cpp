// Redirect esp_log output: TinyUSB CDC when enabled, else UART0 when we own it
// (CONFIG_ESP_CONSOLE_NONE + log-uart). Do NOT use CONFIG_ESP_CONSOLE_USB_CDC
// (ROM stack) - it conflicts with TinyUSB.

#include <cstdarg>
#include <cstdio>

extern "C" {
#include "esp_log.h"
#include "sdkconfig.h"
}

#if CONFIG_TINYUSB_CDC_ENABLED

#include "fujinet/platform/esp32/tinyusb_cdc.h"

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

#else

// No TinyUSB CDC. When secondary console is USB Serial JTAG, the default log output
// already goes to the USB port the user is watching; we must NOT replace it or logs vanish.
// Only install UART0 and redirect when there is no secondary (true UART-only board).
#include "driver/uart.h"

namespace {

constexpr uart_port_t UART_LOG_PORT = UART_NUM_0;
constexpr std::size_t LOG_BUF_SIZE = 512;

static int uart_log_vprintf(const char* fmt, va_list args)
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
    (void)uart_write_bytes(UART_LOG_PORT, buf, len);
    return n;
}

} // namespace

#endif // CONFIG_TINYUSB_CDC_ENABLED

extern "C" void platform_install_log_output(void)
{
#if CONFIG_TINYUSB_CDC_ENABLED
    esp_log_set_vprintf(&tinyusb_log_vprintf);
    const auto port = (CONFIG_FN_ESP_CONSOLE_CDC_NUM == 0)
        ? fujinet::platform::esp32::UsbCdcAcmPort::Port0
        : fujinet::platform::esp32::UsbCdcAcmPort::Port1;
    const char banner[] = "\r\n*** FujiNet logs (this port; build with FN_DEBUG for app logs) ***\r\n";
    fujinet::platform::esp32::write_cdc_port(port, banner, sizeof(banner) - 1);
    ESP_LOGI("nio", "Log output redirected to this CDC port.");
#else
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    // Primary console is NONE, secondary is USB Serial JTAG. Logs already go to USB
    // (the port pio monitor uses). Do NOT call esp_log_set_vprintf or we redirect
    // logs to UART0 and they disappear from the port the user is watching.
    (void)0;
#else
    // No USB Serial JTAG: install UART0 and redirect esp_log so logs appear on UART0.
    constexpr int rx_buf = 1024;
    constexpr int tx_buf = 256;
    esp_err_t err = uart_driver_install(UART_NUM_0, rx_buf, tx_buf, 0, nullptr, 0);
    if (err == ESP_OK) {
        uart_config_t cfg = {};
        cfg.baud_rate = 115200;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;
        (void)uart_param_config(UART_NUM_0, &cfg);
        (void)uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    esp_log_set_vprintf(&uart_log_vprintf);
    esp_log_level_set("*", ESP_LOG_INFO);
    const char banner[] = "\r\n*** FujiNet logs + CLI on UART0 (type help for CLI) ***\r\n";
    (void)uart_write_bytes(UART_NUM_0, banner, sizeof(banner) - 1);
    ESP_LOGI("nio", "Log output on UART0.");
#endif
#endif
}
