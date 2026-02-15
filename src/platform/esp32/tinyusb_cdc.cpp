#include "fujinet/platform/esp32/tinyusb_cdc.h"

#include "fujinet/core/logging.h"

extern "C" {
#include "sdkconfig.h"
}

#if CONFIG_TINYUSB_CDC_ENABLED
extern "C" {
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
}
#endif

namespace fujinet::platform::esp32 {

static const char* TAG = "platform";

#if CONFIG_TINYUSB_CDC_ENABLED
static bool s_driver_inited = false;
static bool s_cdc0_inited = false;
#if CONFIG_TINYUSB_CDC_COUNT > 1
static bool s_cdc1_inited = false;
#endif
#endif

bool ensure_tinyusb_driver()
{
#if !CONFIG_TINYUSB_CDC_ENABLED
    return false;
#else
    if (s_driver_inited) {
        return true;
    }

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);

    // ESP-IDF TinyUSB returns ESP_ERR_INVALID_STATE if already installed.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        FN_LOGE(TAG, "tinyusb_driver_install failed: %d", (int)err);
        return false;
    }

    s_driver_inited = true;
    return true;
#endif
}

bool ensure_tinyusb_cdc_acm(UsbCdcAcmPort port)
{
#if !CONFIG_TINYUSB_CDC_ENABLED
    (void)port;
    return false;
#else
    if (!ensure_tinyusb_driver()) {
        return false;
    }

#if CONFIG_TINYUSB_CDC_COUNT <= 1
    if (port == UsbCdcAcmPort::Port1) {
        return false;
    }
#endif

    if (port == UsbCdcAcmPort::Port0 && s_cdc0_inited) return true;
#if CONFIG_TINYUSB_CDC_COUNT > 1
    if (port == UsbCdcAcmPort::Port1 && s_cdc1_inited) return true;
#endif

    static auto to_itf = [](UsbCdcAcmPort p) -> tinyusb_cdcacm_itf_t {
        return (p == UsbCdcAcmPort::Port0) ? TINYUSB_CDC_ACM_0 : TINYUSB_CDC_ACM_1;
    };

    tinyusb_config_cdcacm_t acm_cfg = {};
    acm_cfg.cdc_port                     = to_itf(port);
    acm_cfg.callback_rx                  = nullptr;
    acm_cfg.callback_rx_wanted_char      = nullptr;
    acm_cfg.callback_line_state_changed  = nullptr;
    acm_cfg.callback_line_coding_changed = nullptr;

    esp_err_t err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        FN_LOGE(TAG, "tinyusb_cdcacm_init(port=%u) failed: %d", (unsigned)port, (int)err);
        return false;
    }

    if (port == UsbCdcAcmPort::Port0) s_cdc0_inited = true;
#if CONFIG_TINYUSB_CDC_COUNT > 1
    if (port == UsbCdcAcmPort::Port1) s_cdc1_inited = true;
#endif

    return true;
#endif
}

} // namespace fujinet::platform::esp32

// C-linkage: no logging inside to avoid recursion during USB driver init.
extern "C" void platform_usb_console_early_init(void)
{
#if !CONFIG_TINYUSB_CDC_ENABLED
    (void)0;
#else
    (void)fujinet::platform::esp32::ensure_tinyusb_driver();
#if defined(CONFIG_ESP_CONSOLE_USB_CDC) && CONFIG_ESP_CONSOLE_USB_CDC
    {
        const auto port = (CONFIG_ESP_CONSOLE_USB_CDC_NUM == 0)
            ? fujinet::platform::esp32::UsbCdcAcmPort::Port0
            : fujinet::platform::esp32::UsbCdcAcmPort::Port1;
        (void)fujinet::platform::esp32::ensure_tinyusb_cdc_acm(port);
    }
#endif
#endif
}


