#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

#include "fujinet/build/profile.h"
#include "fujinet/core/core.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/fuji_device_ids.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/fuji_device_factory.h"

#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/posix/filesystem_factory.h"

// Quick forward declaration (we’ll make a proper header later).
namespace fujinet {
    std::string_view version();
}

using namespace fujinet;
using namespace fujinet::io::protocol;

int main()
{
    std::cout << "fujinet-nio starting (POSIX app)\n";
    std::cout << "Version: " << fujinet::version() << "\n";

    fs::StorageManager storage;
    auto hostFs = platform::posix::create_host_filesystem("./fujinet-root", "host");
    storage.registerFileSystem(std::move(hostFs));

    core::FujinetCore core;

    // 2. Determine build profile.
    auto profile = build::current_build_profile();
    std::cout << "Build profile: " << profile.name << "\n";

    // Reset hook — version 1: just log; later we can add restart loop.
    fujinet::platform::FujiDeviceHooks hooks{
        .onReset = []{
            std::cout << "[FujiDevice] Reset requested (POSIX)"
                      << std::endl;
            // TODO: set a restart flag or similar
        }
    };

    {
        auto dev = platform::create_fuji_device(profile, hooks);

        constexpr io::DeviceID fujiDeviceId =
            static_cast<io::DeviceID>(FujiDeviceId::FujiNet);

        bool ok = core.deviceManager().registerDevice(fujiDeviceId, std::move(dev));
        if (!ok) {
            std::cerr << "Failed to register FujiDevice on DeviceID "
                      << static_cast<unsigned>(fujiDeviceId) << "\n";
            return 1;
        }
    }

    // 3. Create a Channel appropriate for this profile (PTY, RS232, etc.).
    auto channel = platform::create_channel_for_profile(profile);
    if (!channel) {
        std::cerr << "Failed to create Channel for profile\n";
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
    std::cout << "fujinet-nio exiting.\n";
    return 0;
}

