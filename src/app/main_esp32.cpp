#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_system.h" // esp_restart


extern "C" {
#include "nvs_flash.h"
}

#include "fujinet/build/profile.h"
#include "fujinet/console/console_engine.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/net/network_link_monitor.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/esp32/fs_factory.h"
#include "fujinet/platform/esp32/fs_init.h"
#include "fujinet/platform/esp32/sntp_service.h"
#include "fujinet/platform/esp32/wifi_link.h"
#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/platform/fuji_device_factory.h"

#include "fujinet/core/logging.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/diag/diagnostic_registry.h"

#include <unistd.h>

static const char* TAG = "nio";

using namespace fujinet;
using namespace fujinet::io::protocol;

namespace {
struct Esp32Services {
    fujinet::io::FujiDevice* fuji{nullptr};

    fujinet::core::SystemEvents* events{nullptr};

    std::unique_ptr<fujinet::platform::esp32::Esp32WifiLink> wifi;
    std::unique_ptr<fujinet::net::NetworkLinkMonitor> wifiMon;

    // Starts SNTP when NetworkEvent::GotIp occurs.
    std::unique_ptr<fujinet::platform::esp32::SntpService> sntp;

    bool phase1_started{false};

    void init_phase0(fujinet::core::FujinetCore& core)
    {
        events = &core.events();
        // Construct services that subscribe to events (even if Wi-Fi may be disabled).
        sntp = std::make_unique<fujinet::platform::esp32::SntpService>(*events);
    }

    void start_phase1(fujinet::core::FujinetCore& core)
    {
        if (phase1_started || !fuji || !events) return;
        phase1_started = true;

        // Load config now (not on phase 0 path)
        fuji->start();
        const auto& cfg = fuji->config();

        if (cfg.wifi.enabled && !cfg.wifi.ssid.empty()) {
            wifi = std::make_unique<fujinet::platform::esp32::Esp32WifiLink>();
            wifi->init();
            wifi->connect(cfg.wifi.ssid, cfg.wifi.passphrase);

            // Monitor publishes NetworkEvent transitions based on wifi state/ip.
            wifiMon = std::make_unique<fujinet::net::NetworkLinkMonitor>(*events, *wifi);
        }

        // start all the devices that can be delayed
        // We can now check if they should be started too
        fujinet::core::register_clock_device(core);
        fujinet::core::register_network_device(core);
        if (cfg.modem.enabled)
            fujinet::core::register_modem_device(core);
    
    }

    void poll()
    {
        if (wifi) wifi->poll();
        if (wifiMon) wifiMon->poll();
    }
};
} // namespace
    

extern "C" void fujinet_core_task(void* arg)
{
    core::FujinetCore core;
    Esp32Services services;

    // Diagnostics + console (cooperative; keep in the core task to avoid races).
    fujinet::diag::DiagnosticRegistry diagRegistry;
    auto coreDiag = fujinet::diag::create_core_diagnostic_provider(core);
    auto netDiag  = fujinet::diag::create_network_diagnostic_provider(core);
    auto diskDiag = fujinet::diag::create_disk_diagnostic_provider(core);
    auto modemDiag = fujinet::diag::create_modem_diagnostic_provider(core);
    diagRegistry.add_provider(*coreDiag);
    diagRegistry.add_provider(*netDiag);
    diagRegistry.add_provider(*diskDiag);
    diagRegistry.add_provider(*modemDiag);

    std::unique_ptr<fujinet::console::IConsoleTransport> consoleTransport;
    std::unique_ptr<fujinet::console::ConsoleEngine> console;
    bool console_running = false;

#if CONFIG_FN_CONSOLE_ENABLE
    consoleTransport = fujinet::console::create_default_console_transport();
    if (consoleTransport) {
        console = std::make_unique<fujinet::console::ConsoleEngine>(diagRegistry, *consoleTransport, core.storageManager());
        console->set_reboot_handler([]{
            esp_restart();
        });
        console_running = true;
        consoleTransport->write_line("fujinet-nio diagnostic console (type: help)");
    }
#endif

    services.init_phase0(core);
    
    if (auto flashFs = platform::esp32::create_flash_filesystem()) {
        core.storageManager().registerFileSystem(std::move(flashFs));
    }

    if (auto sdFs = platform::esp32::create_sdcard_filesystem()) {
        core.storageManager().registerFileSystem(std::move(sdFs));
    }

    auto profile = build::current_build_profile();

    // Create a Channel appropriate for this profile (TinyUSB CDC, etc.).
    auto channel = platform::create_channel_for_profile(profile);
    if (!channel) {
        FN_LOGE(TAG, "Failed to create Channel for profile");
        vTaskDelete(nullptr);
        return;
    }

    // Set up transports based on profile (FujiBus, SIO, etc.).
    core::setup_transports(core, *channel, profile);
    FN_ELOG("transport initialized");

    {
        auto dev = platform::create_fuji_device(core, profile);

        // Keep a non-owning pointer for phase-1 start.
        if (auto* fuji = dynamic_cast<fujinet::io::FujiDevice*>(dev.get())) {
            services.fuji = fuji;
        } else {
            FN_LOGE(TAG, "create_fuji_device() did not return a FujiDevice; Wi-Fi/config start disabled");
            services.fuji = nullptr;
        }

        io::DeviceID fujiDeviceId = to_device_id(WireDeviceId::FujiNet);
        
        FN_ELOG("Registering FujiDevice on DeviceID %u", static_cast<unsigned>(fujiDeviceId));
        bool ok = core.deviceManager().registerDevice(fujiDeviceId, std::move(dev));
        if (!ok) {
            FN_LOGE(TAG, "Failed to register FujiDevice on DeviceID %u",
                static_cast<unsigned>(fujiDeviceId));
        }
    }

    // If we load config at this point to find out if the services should be enabled or not, it adds 80ms before the main loop starts
    fujinet::core::register_file_device(core);
    fujinet::core::register_disk_device(core);

    const std::uint64_t phase1_at = core.tick_count() + 100;
    
    FN_ELOG("[%u ms] starting main loop", (unsigned)(esp_timer_get_time()/1000));

    // Run the core loop forever.
    for (;;) {
        core.tick();

        // start phase-1 services after a small delay
        if (!services.phase1_started && core.tick_count() >= phase1_at) {
            services.start_phase1(core);
        }

        services.poll();

        if (console_running) {
            console_running = console->step(0);
        }

// Do this later when we want to check the water mark
// #if defined(FN_DEBUG)
//         if (core.tick_count() % 100 == 0) {
//             UBaseType_t hw = uxTaskGetStackHighWaterMark(nullptr);
//             // hw is MINIMUM free stack (in words) since task start
//             FN_LOGD(TAG, "fujinet_core stack high-water mark: %u words free", hw);
//         }
// #endif

        vTaskDelay(pdMS_TO_TICKS(20));
    }
    FN_LOGI(TAG, "core task exiting");
}

extern "C" void app_main(void)
{
    // Global default: be strict
    esp_log_level_set("*", ESP_LOG_ERROR);

    // Turn our own tags back up a bit
    esp_log_level_set("config",      ESP_LOG_INFO);
    esp_log_level_set("clock",       ESP_LOG_INFO);
    esp_log_level_set("core",        ESP_LOG_INFO);
    esp_log_level_set("events",      ESP_LOG_INFO);
    esp_log_level_set("fs",          ESP_LOG_INFO);
    esp_log_level_set("io",          ESP_LOG_INFO);
    esp_log_level_set("nio",         ESP_LOG_INFO);
    esp_log_level_set("nio-wifi",    ESP_LOG_INFO);
    esp_log_level_set("platform",    ESP_LOG_INFO);
    esp_log_level_set("service",     ESP_LOG_INFO);
    esp_log_level_set("byte_legacy", ESP_LOG_INFO);

    // Silence noisy ESP components we care about:
    esp_log_level_set("heap_init",   ESP_LOG_ERROR);
    esp_log_level_set("spi_flash",   ESP_LOG_ERROR);
    esp_log_level_set("sleep_gpio",  ESP_LOG_ERROR);
    esp_log_level_set("app_init",    ESP_LOG_ERROR);
    esp_log_level_set("efuse_init",  ESP_LOG_ERROR);
    esp_log_level_set("octal_psram", ESP_LOG_ERROR);
    esp_log_level_set("cpu_start",   ESP_LOG_ERROR);
    esp_log_level_set("main_task",   ESP_LOG_ERROR);
    esp_log_level_set("wifi",        ESP_LOG_ERROR);

    // TinyUSB glue:
    esp_log_level_set("tusb_desc",   ESP_LOG_ERROR);
    esp_log_level_set("TinyUSB",     ESP_LOG_ERROR);

    FN_ELOG("fujinet-nio - (ESP32-S3 / ESP-IDF) starting up");

    // Platform bootstrap: NVS init (required by Wi-Fi and other ESP-IDF subsystems).
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            FN_LOGW(TAG, "NVS init needs erase (err=%d), erasing", (int)err);
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        if (err != ESP_OK) {
            FN_LOGE(TAG, "nvs_flash_init failed: %d", (int)err);
            // Continue boot; Wi-Fi will fail later if requested.
        }
    }

    // Initialize the TCP/IP stack + default event loop early so network protocols
    // cannot crash if invoked before Wi-Fi link bring-up (phase-1).
    {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            FN_LOGE(TAG, "esp_netif_init failed: %d", (int)err);
        }

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            FN_LOGE(TAG, "esp_event_loop_create_default failed: %d", (int)err);
        }
    }

    if (!fujinet::platform::esp32::init_littlefs()) {
        FN_LOGE(TAG, "Failed to initialise LittleFS; config persistence will not work.");
        // ... what to do?
    }

    if (!fujinet::platform::esp32::init_sdcard_spi()) {
        FN_LOGE(TAG, "Failed to initialise SD card.");
        // ... what to do?
    }

    xTaskCreate(
        &fujinet_core_task,
        "fujinet_core",
        // 4176, // would like this to be 4096, but yaml-cpp saving blows up. Might need to think about its long term usage!
        // 4176 was as low as I could go without it blowing up, but that wouldn't give us much room for anything else, so simply doubling to 8k for now
        8192,
        nullptr,
        5,
        nullptr
    );
}
