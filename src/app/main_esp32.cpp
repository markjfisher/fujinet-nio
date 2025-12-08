#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "fujinet/build/profile.h"
#include "fujinet/core/core.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/fuji_device_ids.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/esp32/fs_init.h"
#include "fujinet/platform/esp32/fs_factory.h"
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
    if (auto flashFs = platform::esp32::create_flash_filesystem()) {
        core.storageManager().registerFileSystem(std::move(flashFs));
    }

    // Register SD FS (optional; may be nullptr if SD not present/mounted)
    if (auto sdFs = platform::esp32::create_sdcard_filesystem()) {
        core.storageManager().registerFileSystem(std::move(sdFs));
    }

    // 1. Determine build profile.
    auto profile = build::current_build_profile();
    FN_LOGI(TAG, "Build profile: %.*s", static_cast<int>(profile.name.size()), profile.name.data());

    // 2. Register FujiDevice
    {
        auto dev = platform::create_fuji_device(core, profile);
        // FujiDeviceId::FujiNet is a FujiBus concept; the routing layer
        // should map it to DeviceID, but for now we can just pick one.
        constexpr io::DeviceID fujiDeviceId = static_cast<io::DeviceID>(FujiDeviceId::FujiNet);

        bool ok = core.deviceManager().registerDevice(fujiDeviceId, std::move(dev));
        if (!ok) {
            FN_LOGE(TAG, "Failed to register FujiDevice on DeviceID %u",
                     static_cast<unsigned>(fujiDeviceId));
        }
    }

    // 3. Create a Channel appropriate for this profile (TinyUSB CDC, etc.).
    auto channel = platform::create_channel_for_profile(profile);
    if (!channel) {
        FN_LOGE(TAG, "Failed to create Channel for profile");
        vTaskDelete(nullptr);
        return;
    }

    // 4. Set up transports based on profile (FujiBus, SIO, etc.).
    core::setup_transports(core, *channel, profile);

    FN_LOGI(TAG, "fujinet-nio core task starting (transport initialized)");

    // 5. Run the core loop forever.
    for (;;) {
        core.tick();

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
}

extern "C" void app_main(void)
{
    // Global default: be strict
    esp_log_level_set("*", ESP_LOG_ERROR);

    // Turn our own tags back up a bit
    esp_log_level_set("nio", ESP_LOG_INFO);
    esp_log_level_set("fs_init", ESP_LOG_INFO);
    esp_log_level_set("config_yaml", ESP_LOG_WARN); // or INFO if you want

    // Silence noisy ESP components we care about:
    esp_log_level_set("heap_init", ESP_LOG_ERROR);
    esp_log_level_set("spi_flash", ESP_LOG_ERROR);
    esp_log_level_set("sleep_gpio", ESP_LOG_ERROR);
    esp_log_level_set("app_init", ESP_LOG_ERROR);
    esp_log_level_set("efuse_init", ESP_LOG_ERROR);
    esp_log_level_set("octal_psram", ESP_LOG_ERROR);
    esp_log_level_set("cpu_start", ESP_LOG_ERROR);
    esp_log_level_set("main_task", ESP_LOG_ERROR);

    // TinyUSB glue:
    esp_log_level_set("tusb_desc", ESP_LOG_ERROR);
    esp_log_level_set("TinyUSB",   ESP_LOG_ERROR);

    FN_LOGI(TAG, "fujinet-nio (ESP32-S3 / ESP-IDF) starting up...");

    if (!fujinet::platform::esp32::init_littlefs()) {
        FN_LOGE(TAG, "Failed to initialise LittleFS; config persistence will not work.");
        // ... what to do?
    }
    // unlink("/fujifs/fujinet.yaml");
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
