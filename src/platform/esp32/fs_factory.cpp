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

std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem() {
    fujinet::fs::TnfsClientFactory factory = [](const fujinet::fs::TnfsEndpoint& endpoint)
        -> std::unique_ptr<fujinet::tnfs::ITnfsClient>
    {
        auto channel = fujinet::platform::create_udp_channel(endpoint.host, endpoint.port);
        return fujinet::tnfs::make_udp_tnfs_client(std::move(channel));
    };

    return fujinet::fs::make_tnfs_filesystem(std::move(factory));
}

} // namespace fujinet::platform::esp32
