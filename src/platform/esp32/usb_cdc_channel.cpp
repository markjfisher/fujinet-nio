#include "fujinet/platform/esp32/usb_cdc_channel.h"
#include "fujinet/core/logging.h"

#include <cstddef>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
static bool s_cdc0_inited = false;
static bool s_cdc1_inited = false;

static void ensure_tinyusb_init()
{
    if (s_tinyusb_inited) {
        return;
    }

    // Use the default TinyUSB device config provided by esp_tinyusb v2.x
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        FN_LOGE(TAG, "tinyusb_driver_install failed: %d", static_cast<int>(err));
        return;
    }

    // Init CDC ACM ports on-demand; see ensure_cdc_port_init().

    s_tinyusb_inited = true;
}

static tinyusb_cdcacm_itf_t to_itf_from_cfg(int idx)
{
    return (idx == 0) ? TINYUSB_CDC_ACM_0 : TINYUSB_CDC_ACM_1;
}

static void ensure_cdc_port_init(tinyusb_cdcacm_itf_t itf)
{
    ensure_tinyusb_init();
    if (!s_tinyusb_inited) {
        return;
    }

    if (itf == TINYUSB_CDC_ACM_0 && s_cdc0_inited) return;
    if (itf == TINYUSB_CDC_ACM_1 && s_cdc1_inited) return;

    tinyusb_config_cdcacm_t acm_cfg = {};
    acm_cfg.cdc_port                     = itf;
    acm_cfg.callback_rx                  = nullptr;
    acm_cfg.callback_rx_wanted_char      = nullptr;
    acm_cfg.callback_line_state_changed  = nullptr;
    acm_cfg.callback_line_coding_changed = nullptr;

    esp_err_t err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        FN_LOGE(TAG, "tinyusb_cdcacm_init(itf=%d) failed: %d", (int)itf, static_cast<int>(err));
        return;
    }

    if (itf == TINYUSB_CDC_ACM_0) s_cdc0_inited = true;
    if (itf == TINYUSB_CDC_ACM_1) s_cdc1_inited = true;
}

UsbCdcChannel::UsbCdcChannel()
{
    ensure_tinyusb_init();
    ensure_cdc_port_init(to_itf_from_cfg(CONFIG_FN_FUJIBUS_USB_CDC_PORT));
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

    const tinyusb_cdcacm_itf_t itf = to_itf_from_cfg(CONFIG_FN_FUJIBUS_USB_CDC_PORT);
    ensure_cdc_port_init(itf);

    size_t rx_size = 0;
    esp_err_t err = tinyusb_cdcacm_read(
        itf,
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

    const tinyusb_cdcacm_itf_t itf = to_itf_from_cfg(CONFIG_FN_FUJIBUS_USB_CDC_PORT);
    ensure_cdc_port_init(itf);

    const std::uint8_t* p = buffer;
    std::size_t remaining = len;

    // Keep this bounded so we never deadlock if the host disappears.
    // (Tune if needed; 250ms is plenty for CDC flush cadence.)
    const TickType_t start = xTaskGetTickCount();
    const TickType_t max_wait_ticks = pdMS_TO_TICKS(250);

    while (remaining > 0) {
        // Queue as much as TinyUSB will accept right now.
        size_t queued = tinyusb_cdcacm_write_queue(
            itf,
            const_cast<std::uint8_t*>(p),
            remaining
        );

        if (queued > 0) {
            p += queued;
            remaining -= queued;

            // Flush what we just queued.
            (void)tinyusb_cdcacm_write_flush(itf, 0);
            continue;
        }

        // Nothing queued: flush and yield briefly to let USB ISR/task drain.
        (void)tinyusb_cdcacm_write_flush(itf, 0);
        vTaskDelay(1);

        if ((xTaskGetTickCount() - start) > max_wait_ticks) {
            // We *intentionally* drop the remainder rather than block forever.
            // Your higher layer should treat this as "link broken" via timeouts.
            // If you want, we can also promote this to an error signal later.
            return;
        }
    }
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