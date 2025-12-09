#include "fujinet/platform/fuji_device_factory.h"

#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/core/core.h"
#include "fujinet/core/logging.h"

extern "C" {
#include "esp_system.h"  // esp_restart
}

namespace fujinet::platform {

const char* TAG = "fuji_device_factory";

using fujinet::io::FujiDevice;
using fujinet::io::VirtualDevice;

std::unique_ptr<VirtualDevice>
create_fuji_device(fujinet::core::FujinetCore& core,
                   const build::BuildProfile&  /*profile*/,
                   const FujiDeviceHooks&      hooks)
{
    FN_LOGI(TAG, "Creating FujiDevice");

    FujiDevice::ResetHandler resetHandler;

    if (hooks.onReset) {
        resetHandler = hooks.onReset;
    } else {
        resetHandler = [] {
            esp_restart();
        };
    }

    FN_LOGI(TAG, "Creating config store.");
    auto store = create_fuji_config_store(core.storageManager());

    return std::make_unique<FujiDevice>(
        std::move(resetHandler),
        std::move(store)
    );
}

} // namespace fujinet::platform