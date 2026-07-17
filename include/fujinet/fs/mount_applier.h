#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "fujinet/config/fuji_config.h"

namespace fujinet {

// Forward declarations
namespace fs {
class StorageManager;
}

namespace disk {
class DiskService;
struct MountOptions;
}

namespace config {
struct BootConfig;
struct MountConfig;
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

/**
 * @brief Applies persisted config mounts to runtime disk slots
 * 
 * This function implements the "mount all" equivalent for fujinet-nio.
 * It iterates through the FujiConfig mounts and mounts each enabled
 * mount to its corresponding disk slot.
 * 
 * @param diskService The disk::DiskService to mount to
 * @param storage The fs::StorageManager for URI resolution
 * @param mounts The vector of config::MountConfig from FujiConfig
 * @return Number of mounts successfully applied
 */
std::size_t apply_config_mounts(
    disk::DiskService& diskService,
    fs::StorageManager& storage,
    const std::vector<config::MountConfig>& mounts
);

/**
 * @brief Applies persisted config mounts, skipping runtime disk slots reserved by bootstrap policy.
 *
 * Used when boot/config mode has already installed a boot disk into a runtime
 * unit. Persisted mounts remain user slot configuration and must not replace
 * that boot disk before the host has a chance to read it.
 *
 * @param excludedRuntimeSlots 0-based DiskService slot indexes to leave untouched
 */
std::size_t apply_config_mounts_excluding(
    disk::DiskService& diskService,
    fs::StorageManager& storage,
    const std::vector<config::MountConfig>& mounts,
    const std::vector<std::size_t>& excludedRuntimeSlots
);

} // namespace fujinet
