#include "fujinet/fs/mount_applier.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/disk_types.h"
#include "fujinet/core/logging.h"

#include <algorithm>
#include <string>

namespace fujinet {

static constexpr const char* TAG = "mount";

std::size_t apply_boot_mount(
    disk::DiskService& diskService,
    fs::StorageManager& storage,
    const config::BootConfig& boot,
    std::size_t activeDiskUnit)
{
    if (boot.mode != config::BootMode::Config) {
        FN_LOGI(TAG, "Boot mode is not config; no boot disk mount applied");
        return 0;
    }

    if (boot.configUri.empty()) {
        FN_LOGW(TAG, "Boot mode is config but boot config_uri is empty");
        return 0;
    }

    if (activeDiskUnit >= diskService.slot_count()) {
        FN_LOGW(TAG,
                "Boot active disk unit %zu exceeds maximum units %zu",
                activeDiskUnit,
                diskService.slot_count());
        return 0;
    }

    auto [fs, resolvedPath] = storage.resolveUri(boot.configUri);
    if (!fs) {
        FN_LOGW(TAG, "Could not resolve boot config_uri '%s'", boot.configUri.c_str());
        return 0;
    }

    if (resolvedPath.empty()) {
        FN_LOGW(TAG, "Resolved boot config_uri '%s' to an empty path", boot.configUri.c_str());
        return 0;
    }

    if (!fs->exists(resolvedPath)) {
        FN_LOGW(TAG,
                "Boot config_uri '%s' resolved to missing path '%s'",
                boot.configUri.c_str(),
                resolvedPath.c_str());
        return 0;
    }

    const std::string mode = boot.readOnly ? "r" : "rw";
    FN_LOGI(TAG,
            "Setting boot config pending mount: active_unit=%zu, uri='%s', mode='%s'",
            activeDiskUnit,
            boot.configUri.c_str(),
            mode.c_str());

    diskService.set_pending_mount(activeDiskUnit, boot.configUri, mode, true, 0);
    return 1;
}

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
        FN_LOGI(TAG, "Setting pending mount: slot %d, uri='%s', mode='%s', enabled=%d sector_size_hint=%u",
                slotIndex,
                mount.uri.c_str(),
                mount.mode.c_str(),
                mount.enabled,
                static_cast<unsigned>(mount.sectorSizeHint));

        diskService.set_pending_mount(
            static_cast<std::size_t>(slotIndex), 
            mount.uri, 
            mount.mode, 
            mount.enabled,
            mount.sectorSizeHint
        );

        applied++;
        FN_LOGI(TAG, "Configured pending mount for slot %d (lazy activation)", slotIndex);
    }

    FN_LOGI(TAG, "Configured %zu/%zu config mounts as pending (lazy)", applied, mounts.size());
    return applied;
}

} // namespace fujinet
