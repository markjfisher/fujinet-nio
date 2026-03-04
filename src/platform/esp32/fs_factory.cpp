#include "fujinet/fs/fs_stdio.h"
#include "fujinet/platform/esp32/fs_factory.h"
#include "fujinet/platform/esp32/fs_init.h"
#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/esp32/udp_channel.h"
#include "fujinet/tnfs/tnfs_protocol.h"

namespace fujinet::platform::esp32 {

using fujinet::fs::FileSystemKind;

static const char* TAG = "fs";

std::unique_ptr<fujinet::fs::IFileSystem> create_flash_filesystem()
{
    // assume init_littlefs() has already mounted /fujifs
    return fujinet::fs::create_stdio_filesystem(
        "/fujifs",
        "flash",
        FileSystemKind::LocalFlash
    );
}

std::unique_ptr<fujinet::fs::IFileSystem> create_sdcard_filesystem()
{
    // once SD is mounted at /sdcard
    return fujinet::fs::create_stdio_filesystem(
        "/sdcard",
        "sd0",
        FileSystemKind::LocalSD
    );
}

std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem(const std::string& host, uint16_t port, const std::string& mountPath, const std::string& user, const std::string& password) {
    auto channel = fujinet::platform::create_udp_channel(host, port);
    auto client = fujinet::tnfs::make_udp_tnfs_client(std::move(channel));

    if (!client->mount(mountPath, user, password)) {
        FN_LOGE(TAG, "Failed to mount TNFS filesystem");
        return nullptr;
    }

    return fujinet::fs::make_tnfs_filesystem(std::move(client));
}

} // namespace fujinet::platform::esp32
