#include "fujinet/platform/fuji_device_factory.h"

#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/core/core.h"

namespace fujinet::platform {

using fujinet::io::FujiDevice;
using fujinet::io::VirtualDevice;

std::unique_ptr<VirtualDevice>
create_fuji_device(core::FujinetCore&         core,
                   const build::BuildProfile& /*profile*/,
                   const FujiDeviceHooks&     hooks)
{
    auto resetHandler = hooks.onReset ? hooks.onReset
                                      : []{}; // no-op fallback

    // POSIX: this will typically choose "host" FS, then ".".
    auto store = create_fuji_config_store(core.storageManager());

    return std::make_unique<FujiDevice>(
        std::move(resetHandler),
        std::move(store)
    );
}

} // namespace fujinet::platform