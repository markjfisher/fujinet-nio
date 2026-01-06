#include "fujinet/platform/esp32/usb_cdc_channel.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/esp32/tinyusb_cdc.h"

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

UsbCdcChannel::UsbCdcChannel()
{
    const auto port = (CONFIG_FN_FUJIBUS_USB_CDC_PORT == 0)
        ? fujinet::platform::esp32::UsbCdcAcmPort::Port0
        : fujinet::platform::esp32::UsbCdcAcmPort::Port1;
    (void)fujinet::platform::esp32::ensure_tinyusb_cdc_acm(port);
}

bool UsbCdcChannel::available()
{
    return fujinet::platform::esp32::ensure_tinyusb_driver();
}

std::size_t UsbCdcChannel::read(std::uint8_t* buffer, std::size_t maxLen)
{
    if (maxLen == 0) {
        return 0;
    }

    const auto port = (CONFIG_FN_FUJIBUS_USB_CDC_PORT == 0)
        ? fujinet::platform::esp32::UsbCdcAcmPort::Port0
        : fujinet::platform::esp32::UsbCdcAcmPort::Port1;
    if (!fujinet::platform::esp32::ensure_tinyusb_cdc_acm(port)) {
        return 0;
    }

    const tinyusb_cdcacm_itf_t itf = (port == fujinet::platform::esp32::UsbCdcAcmPort::Port0)
        ? TINYUSB_CDC_ACM_0
        : TINYUSB_CDC_ACM_1;

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
    if (!buffer || len == 0) {
        return;
    }

    const auto port = (CONFIG_FN_FUJIBUS_USB_CDC_PORT == 0)
        ? fujinet::platform::esp32::UsbCdcAcmPort::Port0
        : fujinet::platform::esp32::UsbCdcAcmPort::Port1;
    if (!fujinet::platform::esp32::ensure_tinyusb_cdc_acm(port)) {
        return;
    }

    const tinyusb_cdcacm_itf_t itf = (port == fujinet::platform::esp32::UsbCdcAcmPort::Port0)
        ? TINYUSB_CDC_ACM_0
        : TINYUSB_CDC_ACM_1;

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