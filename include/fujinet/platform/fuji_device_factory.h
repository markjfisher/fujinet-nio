#pragma once

#include <functional>
#include <memory>

#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/config/build_profile.h"

namespace fujinet::platform {

struct FujiDeviceHooks {
    // Cross-platform “hook” – caller can choose behaviour.
    std::function<void()> onReset;
};

std::unique_ptr<fujinet::io::VirtualDevice>
create_fuji_device(const fujinet::config::BuildProfile& profile,
                   const FujiDeviceHooks& hooks = {});

} // namespace fujinet::platform
