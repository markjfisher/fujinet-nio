#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Later we'll include and bootstrap the shared fujinet-nio core here.
// For now, this just proves the ESP-IDF + PlatformIO wiring works.

extern "C" void app_main(void)
{
    static const char* TAG = "fujinet-nio";

    ESP_LOGI(TAG, "fujinet-nio (ESP32 / ESP-IDF) starting up...");

    // Eventually:
    //   - initialize configuration
    //   - create IODeviceManager / RoutingManager
    //   - set up transports (RS232/SIO/etc.)
    //   - start the main I/O loop in a FreeRTOS task
    //
    // For now: just idle.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
