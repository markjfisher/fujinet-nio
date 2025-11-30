#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "fujinet/core/core.h"

static const char* TAG = "fujinet-nio";

// FreeRTOS task that owns and runs the FujinetCore loop.
extern "C" void fujinet_core_task(void* arg)
{
    fujinet::core::FujinetCore core;

    ESP_LOGI(TAG, "fujinet-nio core task starting");

    for (;;) {
        core.tick();

        // Log occasionally to prove it’s alive without spamming.
        if (core.tick_count() % 50 == 0) {
            ESP_LOGI(TAG, "tick_count=%llu",
                     static_cast<unsigned long long>(core.tick_count()));
        }

        // TODO: tune this delay according to your desired loop frequency.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ESP-IDF entrypoint
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "fujinet-nio (ESP32 / ESP-IDF) starting up...");

    // Start the core in its own task.
    // Stack size & priority are conservative defaults; adjust later.
    xTaskCreate(
        &fujinet_core_task,
        "fujinet_core",
        4096,          // stack size in words, not bytes
        nullptr,       // task parameter
        5,             // priority
        nullptr        // handle (we don't need it yet)
    );
}
