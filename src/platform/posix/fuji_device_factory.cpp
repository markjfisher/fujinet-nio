#include "fujinet/platform/fuji_device_factory.h"

#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/platform/fuji_config_store_factory.h"

namespace fujinet::platform {

using fujinet::io::FujiDevice;

std::unique_ptr<fujinet::io::VirtualDevice>
create_fuji_device(const fujinet::config::BuildProfile& /*profile*/,
                   const FujiDeviceHooks& hooks)
{
    auto resetHandler = hooks.onReset ? hooks.onReset
                                      : []{}; // no-op fallback

    auto store = create_fuji_config_store(".");

    return std::make_unique<FujiDevice>(
        std::move(resetHandler),
        std::move(store)
    );
}

} // namespace fujinet::platform
