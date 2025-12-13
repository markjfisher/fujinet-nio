#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <memory>
#include <thread>

#include "fujinet/build/profile.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/core/logging.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/fuji_device_factory.h"
#include "fujinet/platform/posix/fs_factory.h"

// Quick forward declaration (we’ll make a proper header later).
namespace fujinet {
    std::string_view version();
}

using namespace fujinet;
using namespace fujinet::io::protocol;

static const char* TAG = "nio";

int main()
{
    FN_LOGI(TAG, "fujinet-nio starting (POSIX app)");
    FN_LOGI(TAG, "Version: %s", fujinet::version());

    core::FujinetCore core;

    // 2. Determine build profile.
    auto profile = build::current_build_profile();
    auto profile_name = profile.name;
    FN_LOGI(TAG, "Build profile: %.*s", static_cast<int>(profile_name.size()), profile.name.data());

    // Register a host filesystem rooted at ./fujinet-root
    {
        auto hostFs = fujinet::platform::posix::create_host_filesystem("./fujinet-data");

        if (!hostFs) {
            FN_LOGE(TAG, "Failed to create POSIX host filesystem");
            return 1;
        }

        if (!core.storageManager().registerFileSystem(std::move(hostFs))) {
            FN_LOGE(TAG, "StorageManager refused to register 'host' filesystem");
            return 1;
        }

        FN_LOGI(TAG, "Host filesystem registered as 'host'");
    }

    // Reset hook — version 1: just log; later we can add restart loop.
    fujinet::platform::FujiDeviceHooks hooks{
        .onReset = []{
            FN_LOGI(TAG, "[FujiDevice] Reset requested (POSIX)");
            // TODO: set a restart flag or similar
        }
    };

    {
        auto dev = platform::create_fuji_device(core, profile, hooks);

        io::DeviceID fujiDeviceId = to_device_id(WireDeviceId::FujiNet);

        bool ok = core.deviceManager().registerDevice(fujiDeviceId, std::move(dev));
        if (!ok) {
            FN_LOGE(TAG, "Failed to register FujiDevice on DeviceID %d", static_cast<unsigned>(fujiDeviceId));
            return 1;
        }
    }

    // Register Core Devices
    // TODO: use config to decide if we want to start these or not
    fujinet::core::register_file_device(core);
    fujinet::core::register_clock_device(core);

    // Create a Channel appropriate for this profile (PTY, RS232, etc.).
    auto channel = platform::create_channel_for_profile(profile);
    if (!channel) {
        FN_LOGE(TAG, "Failed to create Channel for profile");
        return 1;
    }

    // 4. Set up transports based on profile (RS232/PTY/etc.).
    core::setup_transports(core, *channel, profile);

    // 5. Run core loop until the process is terminated (Ctrl+C, kill, etc.).
    while (true) {
        core.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // (Unreachable for now, but kept for future clean shutdown logic.)
    FN_LOGI(TAG, "fujinet-nio exiting.");
    return 0;
}

