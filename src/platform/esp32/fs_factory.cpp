#include "fujinet/fs/fs_stdio.h"
#include "fujinet/platform/esp32/fs_factory.h"
#include "fujinet/platform/esp32/fs_init.h"

namespace fujinet::platform::esp32 {

using fujinet::fs::FileSystemKind;

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

} // namespace fujinet::platform::esp32
