#include "fujinet/build/profile.h"

extern "C" {
#include "esp_chip_info.h"
#include "esp_flash.h"
#if CONFIG_SPIRAM_SUPPORT || CONFIG_SPIRAM
#include "esp_psram.h"
#endif
}

namespace fujinet::build {

HardwareCapabilities detect_hardware_capabilities()
{
    HardwareCapabilities caps{};

    // -----------------------------
    // Flash → persistent storage
    // -----------------------------
    uint32_t flashSize = 0;
    if (esp_flash_get_size(nullptr, &flashSize) == ESP_OK) {
        caps.memory.persistentStorageBytes = flashSize; 
    }

    // -----------------------------
    // PSRAM → large memory pool
    // -----------------------------
#if CONFIG_SPIRAM_SUPPORT || CONFIG_SPIRAM
    size_t psram = esp_psram_get_size();
    if (psram > 0) {
        caps.memory.largeMemoryPoolBytes = psram;
        caps.memory.hasDedicatedLargePool = true;
    }
#endif

    // -----------------------------
    // Network
    // -----------------------------
    caps.network.hasLocalNetwork = true;        // ESP32 has TCP/IP stack
    caps.network.managesItsOwnLink = true;      // Wi-Fi config happens on-board
    caps.network.supportsAccessPointMode = true;

    // -----------------------------
    // USB
    // -----------------------------
#if defined(FN_PLATFORM_ESP32S3)
    caps.hasUsbDevice = true; 
    caps.hasUsbHost   = true; 
#endif

    return caps;
}

} // namespace fujinet::build
