#include "fujinet/platform/channel_factory.h"

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/build/profile.h"
#include "fujinet/platform/esp32/usb_cdc_channel.h"

extern "C" {
#include "esp_log.h"
}

namespace fujinet::platform {

static const char* TAG = "channel_factory";

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const build::BuildProfile& profile)
{
    using build::ChannelKind;

    switch (profile.primaryChannel) {
    case ChannelKind::UsbCdcDevice:
        ESP_LOGI(TAG, "Using TinyUSB CDC-ACM channel (UsbCdcDevice)");
        return std::make_unique<esp32::UsbCdcChannel>();

    case ChannelKind::Pty:
        ESP_LOGE(TAG, "Pty channel kind not supported on ESP32");
        return nullptr;

    case ChannelKind::TcpSocket:
        ESP_LOGE(TAG, "TcpSocket channel kind not implemented on ESP32");
        return nullptr;
    }

    ESP_LOGE(TAG, "Unknown ChannelKind in create_channel_for_profile");
    return nullptr;
}

} // namespace fujinet::platform
