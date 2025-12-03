#include "fujinet/platform/fuji_device_factory.h"

#include <memory>

#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/platform/fuji_config_store_factory.h"  // shared factory

extern "C" {
#include "esp_system.h"  // esp_restart
}

namespace fujinet::platform {

using fujinet::io::FujiDevice;
using fujinet::io::VirtualDevice;
using fujinet::core::FujiConfigStore;

std::unique_ptr<VirtualDevice>
create_fuji_device(const fujinet::config::BuildProfile& profile,
                   const FujiDeviceHooks& hooks)
{
    (void)profile; // we don’t use it yet, but might later

    // Reset handler: use hook if provided, otherwise default to esp_restart().
    FujiDevice::ResetHandler resetHandler;
    if (hooks.onReset) {
        resetHandler = hooks.onReset;
    } else {
        resetHandler = [] {
            esp_restart();
        };
    }

    // Platform-level config store factory picks SD vs flash, etc.
    auto store = create_fuji_config_store(/*rootHint*/ {});

    return std::make_unique<FujiDevice>(
        std::move(resetHandler),
        std::move(store)
    );
}

} // namespace fujinet::platform
