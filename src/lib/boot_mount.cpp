#include "fujinet/fs/boot_mount.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/disk_types.h"
#include "fujinet/core/logging.h"

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

} // namespace fujinet
