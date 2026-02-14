#include "fujinet/platform/channel_factory.h"
#include "fujinet/core/logging.h"

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/platform/esp32/usb_cdc_channel.h"
#include "fujinet/platform/esp32/uart_channel.h"

extern "C" {
#include "sdkconfig.h"
}

namespace fujinet::platform {

static const char* TAG = "platform";

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const build::BuildProfile& profile, const config::FujiConfig& /*config*/)
{
    using build::ChannelKind;

    switch (profile.primaryChannel) {

    case ChannelKind::UsbCdcDevice:
#if CONFIG_TINYUSB_CDC_ENABLED
        FN_ELOG("Using TinyUSB CDC-ACM channel for UsbCdcDevice");
        return std::make_unique<esp32::UsbCdcChannel>();
#else
        FN_LOGE(TAG, "UsbCdcDevice selected but TinyUSB CDC is disabled in sdkconfig");
        return nullptr;
#endif

    case ChannelKind::Pty:
        FN_LOGE(TAG, "Pty channel kind not supported on ESP32");
        return nullptr;

    case ChannelKind::TcpSocket:
        FN_LOGE(TAG, "TcpSocket channel kind not implemented on ESP32");
        return nullptr;

    case ChannelKind::UdpSocket:
        FN_LOGE(TAG, "UdpSocket channel kind not supported on ESP32");
        return nullptr;

    case ChannelKind::UartGpio:
        FN_ELOG("Using UartChannel for UartGpio");
        return std::make_unique<esp32::UartChannel>();
    }

    FN_LOGE(TAG, "Unknown ChannelKind in create_channel_for_profile");
    return nullptr;
}

} // namespace fujinet::platform
