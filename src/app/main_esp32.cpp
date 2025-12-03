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
#include "fujinet/platform/fuji_device_factory.h"

static const char* TAG = "nio";

using namespace fujinet;
using namespace fujinet::io::protocol;

extern "C" void fujinet_core_task(void* arg)
{
    core::FujinetCore core;

    // 1. Determine build profile.
    auto profile = build::current_build_profile();
    ESP_LOGI(TAG, "Build profile: %.*s",
             static_cast<int>(profile.name.size()),
             profile.name.data());

    // 2. Register FujiDevice
    {
        auto dev = platform::create_fuji_device(profile);
        // FujiDeviceId::FujiNet is a FujiBus concept; the routing layer
        // should map it to DeviceID, but for now we can just pick one.
        constexpr io::DeviceID fujiDeviceId =
            static_cast<io::DeviceID>(FujiDeviceId::FujiNet);

        bool ok = core.deviceManager().registerDevice(fujiDeviceId, std::move(dev));
        if (!ok) {
            ESP_LOGE(TAG, "Failed to register FujiDevice on DeviceID %u",
                     static_cast<unsigned>(fujiDeviceId));
        }
    }

    // 3. Create a Channel appropriate for this profile (TinyUSB CDC, etc.).
    auto channel = platform::create_channel_for_profile(profile);
    if (!channel) {
        ESP_LOGE(TAG, "Failed to create Channel for profile");
        vTaskDelete(nullptr);
        return;
    }

    // 4. Set up transports based on profile (FujiBus, SIO, etc.).
    core::setup_transports(core, *channel, profile);

    ESP_LOGI(TAG, "fujinet-nio core task starting (transport initialized)");

    // 5. Run the core loop forever.
    for (;;) {
        core.tick();

        // if (core.tick_count() % 50 == 0) {
        //     ESP_LOGI(TAG, "tick_count=%llu",
        //              static_cast<unsigned long long>(core.tick_count()));
        // }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "fujinet-nio (ESP32-S3 / ESP-IDF) starting up...");

    xTaskCreate(
        &fujinet_core_task,
        "fujinet_core",
        4096,
        nullptr,
        5,
        nullptr
    );
}
