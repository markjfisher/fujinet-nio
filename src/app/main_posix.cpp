#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <memory>
#include <thread>

#include "fujinet/build/profile.h"
#include "fujinet/console/console_engine.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/core/logging.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/diag/diagnostic_registry.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/fuji_device_factory.h"
#include "fujinet/platform/posix/fs_factory.h"

// Quick forward declaration (we’ll make a proper header later).
namespace fujinet {
    const char* version();
}

using namespace fujinet;
using namespace fujinet::io::protocol;

static const char* TAG = "nio";

// Helper: run a std::function once after a delay (no templates; keeps loop clean).
struct DeferredOnce {
    std::chrono::steady_clock::time_point start;
    std::chrono::milliseconds delay;
    bool done{false};

    void poll(const std::function<void()>& fn)
    {
        if (done) return;
        if (std::chrono::steady_clock::now() - start >= delay) {
            fn();
            done = true;
        }
    }
};


int main()
{
    FN_LOGI(TAG, "fujinet-nio starting (POSIX app)");
    FN_LOGI(TAG, "Version: %s", fujinet::version());

    core::FujinetCore core;

    // Diagnostics + console (cooperative; no extra threads).
    fujinet::diag::DiagnosticRegistry diagRegistry;
    auto coreDiag = fujinet::diag::create_core_diagnostic_provider(core);
    diagRegistry.add_provider(*coreDiag);

    auto consoleTransport = fujinet::console::create_default_console_transport();
    fujinet::console::ConsoleEngine console(diagRegistry, *consoleTransport);
    consoleTransport->write_line("fujinet-nio diagnostic console (type: help)");

    // POSIX: assume network is available; publish a synthetic GotIp for services.
    {
        fujinet::net::NetworkEvent ev;
        ev.kind = fujinet::net::NetworkEventKind::GotIp;
        ev.gotIp.ip4 = "127.0.0.1";
        core.events().network().publish(ev);
    }

    // Determine build profile.
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

    // Keep a non-owning pointer so we can call start() after the unique_ptr is moved.
    // DeviceManager owns the FujiDevice for the remainder of the process lifetime.
    auto fuji = platform::create_fuji_device(core, profile, hooks);
    io::DeviceID fujiDeviceId = to_device_id(WireDeviceId::FujiNet);

    if (!fuji) {
        FN_LOGE(TAG, "create_fuji_device returned null");
        return 1;
    }

    // We need the concrete FujiDevice to call start(). The factory returns VirtualDevice.
    auto* fujiConcrete = dynamic_cast<fujinet::io::FujiDevice*>(fuji.get());
    if (!fujiConcrete) {
        FN_LOGE(TAG, "Fuji device factory did not return an io::FujiDevice (cannot call start())");
        return 1;
    }

    // DeviceManager owns the FujiDevice for the remainder of the process lifetime.
    bool ok = core.deviceManager().registerDevice(fujiDeviceId, std::move(fuji));
    if (!ok) {
        FN_LOGE(TAG, "Failed to register FujiDevice on DeviceID %d", static_cast<unsigned>(fujiDeviceId));
        return 1;
    }

    // Register Core Devices
    fujinet::core::register_file_device(core);
    fujinet::core::register_clock_device(core);
    fujinet::core::register_network_device(core);

    // Create a Channel appropriate for this profile (PTY, FujiBus, etc.).
    // and set up transports based on profile.
    auto channel = platform::create_channel_for_profile(profile);
    if (!channel) {
        FN_LOGE(TAG, "Failed to create Channel for profile");
        return 1;
    }
    core::setup_transports(core, *channel, profile);

    // Run core loop until the process is terminated (Ctrl+C, kill, etc.).
    DeferredOnce startFuji{std::chrono::steady_clock::now(), std::chrono::milliseconds(50), false};
    bool running = true;
    while (running) {
        core.tick();

        // Defer config loading off the initial startup path (mirrors ESP32 behavior).
        startFuji.poll([&] {
            FN_LOGI(TAG, "[FujiDevice] start() (deferred)");
            fujiConcrete->start();
        });

        running = console.step(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // (Unreachable for now, but kept for future clean shutdown logic.)
    FN_LOGI(TAG, "fujinet-nio exiting.");
    return 0;
}

