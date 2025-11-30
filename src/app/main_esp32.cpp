#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "fujinet/core/core.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/transport/rs232_transport.h"

static const char* TAG = "fujinet-nio";

using namespace fujinet;

// ---------------------------------------------------------
// Dummy implementations to prove the IO pipeline works.
// (Same idea as POSIX, but running under FreeRTOS.)
// ---------------------------------------------------------

class DummyDevice : public io::VirtualDevice {
public:
    io::IOResponse handle(const io::IORequest& request) override {
        io::IOResponse resp;
        resp.id       = request.id;
        resp.deviceId = request.deviceId;
        resp.status   = io::StatusCode::Ok;
        resp.payload  = request.payload;
        // We don't log here to avoid spamming the UART.
        return resp;
    }

    void poll() override {
        // Could blink an LED or similar later.
    }
};

class DummyChannel : public io::Channel {
public:
    bool available() override {
        return false;
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
        (void)buffer;
        (void)maxLen;
        return 0;
    }

    void write(const std::uint8_t* buffer, std::size_t len) override {
        // For now, just log the fact that something would be written.
        ESP_LOGI(TAG, "[DummyChannel] write %u bytes",
                 static_cast<unsigned>(len));
        (void)buffer;
    }
};

// FreeRTOS task that owns and runs the FujinetCore loop.
extern "C" void fujinet_core_task(void* arg)
{
    core::FujinetCore core;

    // Register a dummy device on DeviceID 1.
    {
        auto dev = std::make_unique<DummyDevice>();
        bool ok = core.deviceManager().registerDevice(1, std::move(dev));
        if (!ok) {
            ESP_LOGE(TAG, "Failed to register DummyDevice on DeviceID 1");
        }
    }

    // Create dummy channel + transport and attach to core.
    DummyChannel channel;
    io::Rs232Transport rs232(channel);
    core.addTransport(&rs232);

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

// ESP-IDF entrypoint
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
