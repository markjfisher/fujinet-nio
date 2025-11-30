#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "fujinet/core/core.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/config/build_profile.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/core/channel.h"

static const char* TAG = "nio";

using namespace fujinet;

// Temporary dummy device + channel for ESP32.
// We'll later swap DummyChannel for a real UART/USB/SIO-backed Channel.

class DummyDevice : public io::VirtualDevice {
public:
    io::IOResponse handle(const io::IORequest& request) override {
        io::IOResponse resp;
        resp.id       = request.id;
        resp.deviceId = request.deviceId;
        resp.status   = io::StatusCode::Ok;
        resp.payload  = request.payload;
        return resp;
    }

    void poll() override {}
};

class DummyChannel : public io::Channel {
public:
    bool available() override { return false; }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
        (void)buffer;
        (void)maxLen;
        return 0;
    }

    void write(const std::uint8_t* buffer, std::size_t len) override {
        ESP_LOGI(TAG, "[DummyChannel] write %u bytes",
                 static_cast<unsigned>(len));
        (void)buffer;
    }
};

extern "C" void fujinet_core_task(void* arg)
{
    core::FujinetCore core;

    // 1. Register a dummy device.
    {
        auto dev = std::make_unique<DummyDevice>();
        bool ok = core.deviceManager().registerDevice(1, std::move(dev));
        if (!ok) {
            ESP_LOGE(TAG, "Failed to register DummyDevice on DeviceID 1");
        }
    }

    // 2. Determine build profile and set up transports.
    auto profile = config::current_build_profile();
    ESP_LOGI(TAG, "Build profile: %.*s",
             static_cast<int>(profile.name.size()),
             profile.name.data());

    DummyChannel channel;
    core::setup_transports(core, channel, profile);

    ESP_LOGI(TAG, "fujinet-nio core task starting");

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
