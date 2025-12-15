#include "fujinet/platform/esp32/fs_init.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/esp32/pinmap.h"

extern "C" {
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
}

namespace fujinet::platform::esp32 {

static const char* TAG = "platform";

// Keep a static card handle so we can unmount if needed later
static sdmmc_card_t* s_sdcard = nullptr;

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
            FN_LOGE(TAG, "Failed to mount or format LittleFS");
        } else if (err == ESP_ERR_NOT_FOUND) {
            FN_LOGE(TAG, "LittleFS partition label '%s' not found", conf.partition_label);
        } else {
            FN_LOGE(TAG, "esp_vfs_littlefs_register failed: 0x%x", static_cast<unsigned>(err));
        }
        return false;
    }

    size_t total = 0, used = 0;
    err = esp_littlefs_info(conf.partition_label, &total, &used);
    if (err != ESP_OK) {
        FN_LOGW(TAG, "Failed to get LittleFS info (err=0x%x)", static_cast<unsigned>(err));
    } else {
        FN_ELOG("LittleFS mounted at %s, total=%u, used=%u",
                 conf.base_path,
                 static_cast<unsigned>(total),
                 static_cast<unsigned>(used));
    }

    return true;
}

bool init_sdcard_spi()
{
    using fujinet::platform::esp32::pinmap;

    const auto& pins = pinmap().sd;

    FN_ELOG(
        "Initializing SD card over SPI: MOSI=%d MISO=%d SCK=%d CS=%d",
        pins.mosi, pins.miso, pins.sck, pins.cs);

    // 1) Host config for SDSPI
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    // Host slot is the SPI host (e.g. SPI2_HOST / SPI3_HOST)
    host.slot = SPI2_HOST;  // or SPI3_HOST – just keep it consistent with the bus init

    // 2) SPI bus config – this is where MOSI/MISO/SCK go
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = pins.mosi;
    bus_cfg.miso_io_num = pins.miso;
    bus_cfg.sclk_io_num = pins.sck;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;

    esp_err_t err = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot),
                                       &bus_cfg,
                                       SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        FN_LOGE(TAG, "spi_bus_initialize failed: 0x%x", static_cast<unsigned>(err));
        return false;
    }

    // 3) SDSPI “slot” config – ONLY CS + host id
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(pins.cs);
    slot_config.host_id = static_cast<spi_host_device_t>(host.slot);

    // 4) FAT mount config
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed   = false,
        .max_files                = 32,
        .allocation_unit_size     = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat              = false,
    };

    const char* mount_point = "/sdcard";

    err = esp_vfs_fat_sdspi_mount(mount_point,
                                  &host,
                                  &slot_config,
                                  &mount_config,
                                  &s_sdcard);
    if (err != ESP_OK) {
        FN_LOGW(TAG, "Failed to mount SD card at %s (err=0x%x)",
                mount_point, static_cast<unsigned>(err));
        return false;
    }

    // Optional but handy while debugging:
    sdmmc_card_print_info(stdout, s_sdcard);
    FN_ELOG("SD card mounted at %s", mount_point);
    return true;
}

} // namespace fujinet::platform::esp32
                                            