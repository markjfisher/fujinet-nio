#pragma once

#include <memory>

#include "fujinet/fs/filesystem.h"

namespace fujinet::platform::esp32 {

// Wraps LittleFS (or other flash-backed FS) mounted at /fujifs.
std::unique_ptr<fujinet::fs::IFileSystem> create_flash_filesystem();

// Wraps SD card FS mounted at /sdcard (or similar).
// Returns nullptr if we decide SD isn’t available / not mounted.
std::unique_ptr<fujinet::fs::IFileSystem> create_sdcard_filesystem();

} // namespace fujinet::platform::esp32