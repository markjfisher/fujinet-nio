#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"


extern "C" {
#include "nvs_flash.h"
}

#include "fujinet/build/profile.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/esp32/fs_factory.h"
#include "fujinet/platform/esp32/fs_init.h"
#include "fujinet/platform/esp32/wifi_link.h"
#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/platform/fuji_device_factory.h"

#include "fujinet/core/logging.h"

#include <unistd.h>

static const char* TAG = "nio";

using namespace fujinet;
using namespace fujinet::io::protocol;

namespace {
    struct Esp32Services {
        fujinet::io::FujiDevice* fuji{nullptr};
        std::unique_ptr<fujinet::platform::esp32::Esp32WifiLink> wifi;
        bool phase1_started{false};
    
        void start_phase1()
        {
            if (phase1_started || !fuji) return;
            phase1_started = true;
    
            // Load config now (not on phase 0 path)
            fuji->start();
    
            const auto& cfg = fuji->config();
    
            if (cfg.wifi.enabled && !cfg.wifi.ssid.empty()) {
                wifi = std::make_unique<fujinet::platform::esp32::Esp32WifiLink>();
                wifi->init();
                wifi->connect(cfg.wifi.ssid, cfg.wifi.passphrase);
            }
        }
    
        void poll()
        {
            if (wifi) wifi->poll();
        }
    };
    
} // namespace    

extern "C" void fujinet_core_task(void* arg)
{
    core::FujinetCore core;
    Esp32Services services;
    
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

    // TODO: use config to decide if we want to start these or not
    // HOWEVER they will have to go in Esp32Services, as we delay loading config.
    fujinet::core::register_file_device(core);
    fujinet::core::register_clock_device(core);
    fujinet::core::register_network_device(core);

    const std::uint64_t phase1_at = core.tick_count() + 50;
    
    FN_ELOG("[%u ms] starting main loop", (unsigned)(esp_timer_get_time()/1000));

    // 5. Run the core loop forever.
    for (;;) {
        core.tick();

        // start phase-1 services after a small delay
        if (!services.phase1_started && core.tick_count() >= phase1_at) {
            services.start_phase1();
        }

        services.poll();

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
    esp_log_level_set("fs",          ESP_LOG_INFO);
    esp_log_level_set("io",          ESP_LOG_INFO);
    esp_log_level_set("nio",         ESP_LOG_INFO);
    esp_log_level_set("platform",    ESP_LOG_INFO);
    esp_log_level_set("nio-wifi",    ESP_LOG_INFO);

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
