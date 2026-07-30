#pragma once

#include <cstddef>
#include "fujinet/config/fuji_config.h"

namespace fujinet {

// Forward declarations
namespace fs {
class StorageManager;
}

namespace disk {
class DiskService;
}

namespace config {
struct BootConfig;
}

/**
 * @brief Applies the configured boot/config disk to a runtime disk slot.
 *
 * The boot disk is separate from persisted user slots. It is only applied
 * when boot.mode is config, and is installed as a pending lazy mount on the
 * active disk unit selected by platform/bootstrap policy.
 *
 * @param diskService The disk::DiskService to mount to
 * @param storage The fs::StorageManager for URI resolution
 * @param boot The config::BootConfig from FujiConfig
 * @param activeDiskUnit Runtime DiskService unit index to receive the boot disk
 * @return 1 when a boot mount was configured, otherwise 0
 */
std::size_t apply_boot_mount(
    disk::DiskService& diskService,
    fs::StorageManager& storage,
    const config::BootConfig& boot,
    std::size_t activeDiskUnit
);

} // namespace fujinet
