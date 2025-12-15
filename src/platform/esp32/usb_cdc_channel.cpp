#include "fujinet/platform/esp32/usb_cdc_channel.h"
#include "fujinet/core/logging.h"

#include <cstddef>

extern "C" {
#include "sdkconfig.h"   // <-- add this
}

// this has to be at compiler, as the header files do the check for TINYUSB
#if CONFIG_TINYUSB_CDC_ENABLED

extern "C" {
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
}

namespace fujinet::platform::esp32 {

static const char* TAG = "platform";

static bool s_tinyusb_inited = false;

static void ensure_tinyusb_init()
{
    if (s_tinyusb_inited) {
        return;
    }

    // Use the default TinyUSB device config provided by esp_tinyusb v2.x
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "tinyusb_driver_install failed: %d", static_cast<int>(err));
        return;
    }

    // Configure CDC-ACM port 0.
    // Your local tinyusb_config_cdcacm_t does NOT have rx_unread_buf_sz,
    // and tusb_cdc_acm_init() is deprecated in favour of tinyusb_cdcacm_init().
    tinyusb_config_cdcacm_t acm_cfg = {};
    acm_cfg.cdc_port                     = TINYUSB_CDC_ACM_0;
    acm_cfg.callback_rx                  = nullptr;
    acm_cfg.callback_rx_wanted_char      = nullptr;
    acm_cfg.callback_line_state_changed  = nullptr;
    acm_cfg.callback_line_coding_changed = nullptr;

    // New-style init function suggested by the compiler
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "tinyusb_cdcacm_init failed: %d", static_cast<int>(err));
        return;
    }

    s_tinyusb_inited = true;
}

UsbCdcChannel::UsbCdcChannel()
{
    ensure_tinyusb_init();
}

bool UsbCdcChannel::available()
{
    // We don't have a cheap way to query bytes-available without reading;
    // just say "maybe" if TinyUSB is up. The transport will call read()
    // and get 0 if there's nothing there.
    return s_tinyusb_inited;
}

std::size_t UsbCdcChannel::read(std::uint8_t* buffer, std::size_t maxLen)
{
    if (!s_tinyusb_inited || maxLen == 0) {
        return 0;
    }

    size_t rx_size = 0;
    esp_err_t err = tinyusb_cdcacm_read(
        TINYUSB_CDC_ACM_0,
        buffer,
        maxLen,
        &rx_size
    );

    if (err != ESP_OK || rx_size == 0) {
        return 0;
    }

    return rx_size;
}

void UsbCdcChannel::write(const std::uint8_t* buffer, std::size_t len)
{
    if (!s_tinyusb_inited || !buffer || len == 0) {
        return;
    }

    size_t queued = tinyusb_cdcacm_write_queue(
        TINYUSB_CDC_ACM_0,
        const_cast<std::uint8_t*>(buffer),
        len
    );

    if (queued == 0) {
        return;
    }

    (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

} // namespace fujinet::platform::esp32

#else // !CONFIG_TINYUSB_CDC_ENABLED

namespace fujinet::platform::esp32 {

// Stub implementation when TinyUSB CDC is disabled.
// Keeps the type linkable but does nothing.

UsbCdcChannel::UsbCdcChannel() = default;

bool UsbCdcChannel::available()
{
    return false;
}

std::size_t UsbCdcChannel::read(std::uint8_t* /*buffer*/, std::size_t /*maxLen*/)
{
    return 0;
}

void UsbCdcChannel::write(const std::uint8_t* /*buffer*/, std::size_t /*len*/)
{
    // no-op
}

} // namespace fujinet::platform::esp32

#endif // CONFIG_TINYUSB_CDC_ENABLED