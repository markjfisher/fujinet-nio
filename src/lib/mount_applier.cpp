#include "fujinet/fs/mount_applier.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/disk_types.h"
#include "fujinet/core/logging.h"

#include <algorithm>

namespace fujinet {

static constexpr const char* TAG = "mount";

std::size_t apply_config_mounts(
    disk::DiskService& diskService,
    fs::StorageManager& storage,
    const std::vector<config::MountConfig>& mounts)
{
    std::size_t applied = 0;

    for (const auto& mount : mounts) {
        // Skip disabled mounts
        if (!mount.enabled) {
            FN_LOGD(TAG, "Skipping disabled mount at slot %d", mount.slot);
            continue;
        }

        // Skip empty URIs
        if (mount.uri.empty()) {
            FN_LOGD(TAG, "Skipping mount with empty URI at slot %d", mount.slot);
            continue;
        }

        // Get the effective slot (1-8 -> 0-7 conversion)
        int slotIndex = mount.effective_slot();
        
        // If slot is unassigned (-1), skip this mount (or could assign to next available)
        if (slotIndex < 0) {
            FN_LOGW(TAG, "Mount at slot %d has no valid slot assignment, skipping", mount.slot);
            continue;
        }

        // Skip slots beyond max
        if (static_cast<std::size_t>(slotIndex) >= diskService.slot_count()) {
            FN_LOGW(TAG, "Slot index %d exceeds maximum slots %zu, skipping", 
                    slotIndex, diskService.slot_count());
            continue;
        }

        // Validate that the URI can be resolved to a filesystem (but don't mount yet)
        auto [fs, resolvedPath] = storage.resolveUri(mount.uri);
        if (!fs) {
            FN_LOGW(TAG, "Could not resolve filesystem for URI '%s' at slot %d", 
                    mount.uri.c_str(), mount.slot);
            continue;
        }

        if (resolvedPath.empty()) {
            FN_LOGW(TAG, "Resolved path is empty for URI '%s' at slot %d", 
                    mount.uri.c_str(), mount.slot);
            continue;
        }

        // Store the mount config as pending - this is LAZY, not eager!
        // The actual mount will happen on first read/write access.
        // This allows TNFS servers to be unavailable at startup without blocking.
        FN_LOGI(TAG, "Setting pending mount: slot %d, uri='%s', mode='%s', enabled=%d",
                slotIndex, mount.uri.c_str(), mount.mode.c_str(), mount.enabled);

        diskService.set_pending_mount(
            static_cast<std::size_t>(slotIndex), 
            mount.uri, 
            mount.mode, 
            mount.enabled
        );

        applied++;
        FN_LOGI(TAG, "Configured pending mount for slot %d (lazy activation)", slotIndex);
    }

    FN_LOGI(TAG, "Configured %zu/%zu config mounts as pending (lazy)", applied, mounts.size());
    return applied;
}

} // namespace fujinet
