#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "fujinet/core/core.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/config/build_profile.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/platform/esp32/channel_factory.h"

static const char* TAG = "nio";

using namespace fujinet;

// Dummy device just to exercise the pipeline.
class DummyDevice : public io::VirtualDevice {
public:
    io::IOResponse handle(const io::IORequest& request) override {
        io::IOResponse resp;
        resp.id       = request.id;
        resp.deviceId = request.deviceId;
        resp.status   = io::StatusCode::Ok;
        resp.payload  = request.payload; // echo
        return resp;
    }

    void poll() override {}
};

extern "C" void fujinet_core_task(void* arg)
{
    // Give the USB host a moment to re-enumerate after reset before we
    // print anything. This makes it much more likely you'll see the
    // early logs in your monitor.
    vTaskDelay(pdMS_TO_TICKS(1500));
    // ESP_LOGI(TAG, "core_task starting after 1.5s delay\n");

    core::FujinetCore core;

    // 1. Register a dummy device.
    {
        auto dev = std::make_unique<DummyDevice>();
        bool ok = core.deviceManager().registerDevice(1, std::move(dev));
        if (!ok) {
            ESP_LOGE(TAG, "Failed to register DummyDevice on DeviceID 1");
        }
    }

    // 2. Determine build profile.
    auto profile = config::current_build_profile();
    ESP_LOGI(TAG, "Build profile: %.*s",
             static_cast<int>(profile.name.size()),
             profile.name.data());

    // 3. Create a Channel appropriate for this profile (TinyUSB CDC, etc.).
    auto channel = platform::esp32::create_channel_for_profile(profile);
    if (!channel) {
        ESP_LOGE(TAG, "Failed to create Channel for profile");
        vTaskDelete(nullptr);
        return;
    }

    // 4. Set up transports based on profile (SerialDebug, SIO, etc.).
    core::setup_transports(core, *channel, profile);

    ESP_LOGI(TAG, "fujinet-nio core task starting (transport initialized)");

    // 5. Run the core loop forever.
    for (;;) {
        core.tick();

        if (core.tick_count() % 50 == 0) {
            ESP_LOGI(TAG, "tick_count=%llu",
                     static_cast<unsigned long long>(core.tick_count()));
        }

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
