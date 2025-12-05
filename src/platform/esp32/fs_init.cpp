#include "fujinet/platform/esp32/fs_init.h"

extern "C" {
#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
}

namespace fujinet::platform::esp32 {

static const char* TAG = "fs_init";

bool init_littlefs()
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/fujifs";           // mount point
    conf.partition_label = "storage";     // we'll define this partition
    conf.format_if_mount_failed = true;   // format on first boot if missing
    conf.read_only = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format LittleFS");
        } else if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "LittleFS partition label '%s' not found", conf.partition_label);
        } else {
            ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: 0x%x", static_cast<unsigned>(err));
        }
        return false;
    }

    size_t total = 0, used = 0;
    err = esp_littlefs_info(conf.partition_label, &total, &used);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get LittleFS info (err=0x%x)", static_cast<unsigned>(err));
    } else {
        ESP_LOGI(TAG, "LittleFS mounted at %s, total=%u, used=%u",
                 conf.base_path,
                 static_cast<unsigned>(total),
                 static_cast<unsigned>(used));
    }

    return true;
}

} // namespace fujinet::platform::esp32
                                            