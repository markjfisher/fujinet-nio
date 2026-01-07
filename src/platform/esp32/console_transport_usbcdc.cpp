#include "fujinet/console/console_engine.h"

// NOTE:
// The console must NOT share the same CDC ACM port as FujiBus traffic.
// When CONFIG_TINYUSB_CDC_COUNT=2, we can dedicate:
//   - FujiBus:  CDC ACM port 0 (default)
//   - Console:  CDC ACM port 1 (default)
// Selection/fallback logic lives in console_transport_default.cpp.

#include <string>

extern "C" {
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

#include "fujinet/core/logging.h"
#include "fujinet/platform/esp32/tinyusb_cdc.h"

#if CONFIG_TINYUSB_CDC_ENABLED
extern "C" {
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
}
#endif

namespace fujinet::console {

namespace {

static const char* TAG = "console";

#if CONFIG_TINYUSB_CDC_ENABLED
static tinyusb_cdcacm_itf_t to_itf_from_cfg(int idx)
{
    return (idx == 0) ? TINYUSB_CDC_ACM_0 : TINYUSB_CDC_ACM_1;
}

static bool ensure_console_cdc_ready(tinyusb_cdcacm_itf_t itf)
{
    const auto port = (itf == TINYUSB_CDC_ACM_0)
        ? fujinet::platform::esp32::UsbCdcAcmPort::Port0
        : fujinet::platform::esp32::UsbCdcAcmPort::Port1;
    return fujinet::platform::esp32::ensure_tinyusb_cdc_acm(port);
}

class Esp32UsbCdcConsoleTransport final : public IConsoleTransport {
public:
    explicit Esp32UsbCdcConsoleTransport(tinyusb_cdcacm_itf_t itf)
        : _itf(itf)
    {
        (void)ensure_console_cdc_ready(_itf);
    }

    bool read_line(std::string& out, int timeout_ms) override
    {
        // TinyUSB read is non-blocking; emulate timeout by polling/sleeping.
        const TickType_t to = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        const TickType_t start = xTaskGetTickCount();

        std::uint8_t tmp[64];
        for (;;) {
            if (!ensure_console_cdc_ready(_itf)) {
                return false;
            }

            size_t rx_size = 0;
            esp_err_t err = tinyusb_cdcacm_read(_itf, tmp, sizeof(tmp), &rx_size);
            if (err == ESP_OK && rx_size > 0) {
                for (size_t i = 0; i < rx_size; ++i) {
                    const char ch = static_cast<char>(tmp[i]);
                    if (ch == '\r' || ch == '\n') {
                        out = _buf;
                        _buf.clear();
                        return true;
                    }

                    _buf.push_back(ch);
                    if (_buf.size() > 512) {
                        _buf.clear();
                        out.clear();
                        return true;
                    }
                }
                return false;
            }

            if (timeout_ms == 0) {
                return false;
            }

            if (timeout_ms > 0 && (xTaskGetTickCount() - start) >= to) {
                return false;
            }

            vTaskDelay(1);
        }
    }

    void write(std::string_view s) override
    {
        if (!ensure_console_cdc_ready(_itf)) {
            return;
        }

        const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(s.data());
        std::size_t remaining = s.size();

        const TickType_t start = xTaskGetTickCount();
        const TickType_t max_wait_ticks = pdMS_TO_TICKS(250);

        while (remaining > 0) {
            size_t queued = tinyusb_cdcacm_write_queue(_itf, p, remaining);
            if (queued > 0) {
                p += queued;
                remaining -= queued;
                (void)tinyusb_cdcacm_write_flush(_itf, 0);
                continue;
            }

            (void)tinyusb_cdcacm_write_flush(_itf, 0);
            vTaskDelay(1);

            if ((xTaskGetTickCount() - start) > max_wait_ticks) {
                return;
            }
        }
    }

    void write_line(std::string_view s) override
    {
        write(s);
        write("\r\n");
    }

private:
    tinyusb_cdcacm_itf_t _itf;
    std::string _buf;
};
#endif // CONFIG_TINYUSB_CDC_ENABLED

} // namespace

std::unique_ptr<IConsoleTransport> create_console_transport_usbcdc()
{
#if !CONFIG_FN_CONSOLE_ENABLE
    // Console disabled => do not touch any console Kconfig symbols that may not exist.
    return nullptr;
#elif !CONFIG_FN_CONSOLE_TRANSPORT_USB_CDC
    // Console enabled but not configured for USB CDC.
    return nullptr;
#elif !CONFIG_TINYUSB_CDC_ENABLED
    // USB CDC requested but TinyUSB CDC disabled.
    return nullptr;
#else
    // If the build exposes only 1 CDC interface, we cannot have a dedicated console CDC port.
    if (CONFIG_TINYUSB_CDC_COUNT < 2) {
        return nullptr;
    }

    if (CONFIG_FN_CONSOLE_USB_CDC_PORT == CONFIG_FN_FUJIBUS_USB_CDC_PORT) {
        return nullptr;
    }

    const tinyusb_cdcacm_itf_t itf = to_itf_from_cfg(CONFIG_FN_CONSOLE_USB_CDC_PORT);
    return std::make_unique<Esp32UsbCdcConsoleTransport>(itf);
#endif
}

} // namespace fujinet::console


