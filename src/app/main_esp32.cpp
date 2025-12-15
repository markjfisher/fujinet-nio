#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "fujinet/build/profile.h"
#include "fujinet/core/core.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/device_init.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/esp32/fs_init.h"
#include "fujinet/platform/esp32/fs_factory.h"
#include "fujinet/platform/esp32/wifi_link.h"
#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/platform/fuji_device_factory.h"

#include "fujinet/core/logging.h"

#include <unistd.h>

static const char* TAG = "nio";

using namespace fujinet;
using namespace fujinet::io::protocol;

extern "C" void fujinet_core_task(void* arg)
{
    core::FujinetCore core;

    // Register flash FS
    FN_LOGI(TAG, "Registering flash filesystem");
    if (auto flashFs = platform::esp32::create_flash_filesystem()) {
        core.storageManager().registerFileSystem(std::move(flashFs));
    }

    // Register SD FS (optional; may be nullptr if SD not present/mounted)
    FN_LOGI(TAG, "Registering SD filesystem");
    if (auto sdFs = platform::esp32::create_sdcard_filesystem()) {
        core.storageManager().registerFileSystem(std::move(sdFs));
    }

    for (auto& name : core.storageManager().listNames()) {
        FN_LOGI(TAG, "Registered filesystem: %s", name.c_str());
    }

    // 1. Determine build profile.
    auto profile = build::current_build_profile();
    FN_LOGI(TAG, "Build profile: %.*s", static_cast<int>(profile.name.size()), profile.name.data());

    // 1b. Load config early so platform services (like Wi-Fi) can be started before devices.
    auto configStore = platform::create_fuji_config_store(core.storageManager());
    fujinet::config::FujiConfig cfg{};
    if (configStore) {
        cfg = configStore->load();
    }

    // 1c. Optional Wi-Fi bring-up (STA) from config.
    // Keep lifetime for duration of task.
    std::unique_ptr<fujinet::platform::esp32::Esp32WifiLink> wifi;
    if (cfg.wifi.enabled && !cfg.wifi.ssid.empty()) {
        wifi = std::make_unique<fujinet::platform::esp32::Esp32WifiLink>();
        wifi->init();
        wifi->connect(cfg.wifi.ssid, cfg.wifi.passphrase);
    }

    // 2a. Register FujiDevice
    {
        FN_LOGI(TAG, "Creating FujiDevice");
        auto dev = platform::create_fuji_device(core, profile);
        io::DeviceID fujiDeviceId = to_device_id(WireDeviceId::FujiNet);
        
        FN_LOGI(TAG, "Registering FujiDevice on DeviceID %u", static_cast<unsigned>(fujiDeviceId));
        bool ok = core.deviceManager().registerDevice(fujiDeviceId, std::move(dev));
        if (!ok) {
            FN_LOGE(TAG, "Failed to register FujiDevice on DeviceID %u",
                static_cast<unsigned>(fujiDeviceId));
        }
    }

    // 2b. Register Core Devices
    // TODO: use config to decide if we want to start these or not
    fujinet::core::register_file_device(core);
    fujinet::core::register_clock_device(core);
    fujinet::core::register_network_device(core);

    // 3. Create a Channel appropriate for this profile (TinyUSB CDC, etc.).
    auto channel = platform::create_channel_for_profile(profile);
    if (!channel) {
        FN_LOGE(TAG, "Failed to create Channel for profile");
        vTaskDelete(nullptr);
        return;
    }

    // 4. Set up transports based on profile (FujiBus, SIO, etc.).
    core::setup_transports(core, *channel, profile);

    FN_LOGI(TAG, "core task starting (transport initialized)");

    // 5. Run the core loop forever.
    for (;;) {
        core.tick();
        if (wifi) {
            wifi->poll();
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
    esp_log_level_set("core",        ESP_LOG_INFO);
    esp_log_level_set("fs",          ESP_LOG_INFO);
    esp_log_level_set("io",          ESP_LOG_INFO);
    esp_log_level_set("nio",         ESP_LOG_INFO);
    esp_log_level_set("platform",    ESP_LOG_INFO);

    // Silence noisy ESP components we care about:
    esp_log_level_set("heap_init",   ESP_LOG_ERROR);
    esp_log_level_set("spi_flash",   ESP_LOG_ERROR);
    esp_log_level_set("sleep_gpio",  ESP_LOG_ERROR);
    esp_log_level_set("app_init",    ESP_LOG_ERROR);
    esp_log_level_set("efuse_init",  ESP_LOG_ERROR);
    esp_log_level_set("octal_psram", ESP_LOG_ERROR);
    esp_log_level_set("cpu_start",   ESP_LOG_ERROR);
    esp_log_level_set("main_task",   ESP_LOG_ERROR);

    // TinyUSB glue:
    esp_log_level_set("tusb_desc",   ESP_LOG_ERROR);
    esp_log_level_set("TinyUSB",     ESP_LOG_ERROR);

    FN_LOGI(TAG, "(ESP32-S3 / ESP-IDF) starting up...");

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
