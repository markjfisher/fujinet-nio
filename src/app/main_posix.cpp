#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <memory>
#include <thread>
#include <atomic>
#if __has_include(<sysexits.h>)
#include <sysexits.h> // EX_TEMPFAIL=75
#endif

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
    auto netDiag  = fujinet::diag::create_network_diagnostic_provider(core);
    auto diskDiag = fujinet::diag::create_disk_diagnostic_provider(core);
    auto modemDiag = fujinet::diag::create_modem_diagnostic_provider(core);
    diagRegistry.add_provider(*coreDiag);
    diagRegistry.add_provider(*netDiag);
    diagRegistry.add_provider(*diskDiag);
    diagRegistry.add_provider(*modemDiag);

    auto consoleTransport = fujinet::console::create_default_console_transport();
    fujinet::console::ConsoleEngine console(diagRegistry, *consoleTransport, core.storageManager());
    // Note: for PTY transports, ConsoleEngine prints MOTD/prompt on connect so first-time connects see it.

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

    // Reset hook:
    // - ESP32: typically restarts the MCU
    // - POSIX: we request a controlled restart/exit of the process (supervisor/script can re-launch)
    std::atomic_bool reboot_requested{false};

    fujinet::platform::FujiDeviceHooks hooks{
        .onReset = []{
            // Placeholder overwritten below.
        }
    };

    hooks.onReset = [&]{
        FN_LOGI(TAG, "[FujiDevice] Reset requested (POSIX)");
        reboot_requested.store(true);
    };

    // Wire reboot command from console to the same reset hook used by FujiDevice.
    console.set_reboot_handler(hooks.onReset);

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

    // Load config immediately - transports need it (especially NetSIO config)
    // This is fast (just reading a YAML file) and necessary for proper transport setup
    FN_LOGI(TAG, "[FujiDevice] Loading config for transport setup");
    fujiConcrete->start();
    const auto& config = fujiConcrete->config();

    // Register Core Devices
    fujinet::core::register_file_device(core);
    fujinet::core::register_clock_device(core);
    fujinet::core::register_disk_device(core);
    fujinet::core::register_network_device(core);
    fujinet::core::register_modem_device(core);

    // Create a Channel appropriate for this profile (PTY, FujiBus, etc.).
    // and set up transports based on profile.
    auto channel = platform::create_channel_for_profile(profile, config);
    if (!channel) {
        FN_LOGE(TAG, "Failed to create Channel for profile");
        return 1;
    }
    core::setup_transports(core, *channel, profile, &config);

    // Run core loop until the process is terminated (Ctrl+C, kill, etc.).
    bool running = true;
    while (running) {
        core.tick();

        running = console.step(0);
        if (reboot_requested.load()) {
            // Return a conventional "retry me" code so a wrapper/supervisor can relaunch.
            // Mirrors old FujiNet run scripts and sysexits.h.
#ifdef EX_TEMPFAIL
            FN_LOGI(TAG, "Reboot requested; exiting process with EX_TEMPFAIL=%d (POSIX)", EX_TEMPFAIL);
            return EX_TEMPFAIL;
#else
            FN_LOGI(TAG, "Reboot requested; exiting process with code 75 (POSIX)");
            return 75;
#endif
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // (Unreachable for now, but kept for future clean shutdown logic.)
    FN_LOGI(TAG, "fujinet-nio exiting.");
    return 0;
}

