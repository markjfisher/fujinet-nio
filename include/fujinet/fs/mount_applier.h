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
struct MountConfig;
}

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

} // namespace fujinet
