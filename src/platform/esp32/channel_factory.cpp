#include "fujinet/platform/esp32/channel_factory.h"

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/config/build_profile.h"
#include "fujinet/platform/esp32/usb_cdc_channel.h"

extern "C" {
#include "esp_log.h"
}

namespace fujinet::platform::esp32 {

static const char* TAG = "channel_factory";

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const config::BuildProfile& profile)
{
    using config::TransportKind;

    switch (profile.primaryTransport) {
    case TransportKind::SerialDebug:
        ESP_LOGI(TAG, "Using TinyUSB CDC-ACM channel for SerialDebug");
        return std::make_unique<UsbCdcChannel>();

    case TransportKind::SIO:
    case TransportKind::IEC:
        ESP_LOGE(TAG, "SIO/IEC channels not implemented on ESP32 yet");
        return nullptr;
    }

    return nullptr;
}

} // namespace fujinet::platform::esp32
