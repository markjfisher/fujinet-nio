#pragma once

#include <memory>

#include "fujinet/fs/filesystem.h"

namespace fujinet::platform::esp32 {

// Wraps LittleFS (or other flash-backed FS) mounted at /fujifs.
std::unique_ptr<fujinet::fs::IFileSystem> create_flash_filesystem();

// Wraps SD card FS mounted at /sdcard (or similar).
// Returns nullptr if we decide SD isn’t available / not mounted.
std::unique_ptr<fujinet::fs::IFileSystem> create_sdcard_filesystem();

// Creates a TNFS filesystem instance connected to the specified host
std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem(const std::string& host, uint16_t port, const std::string& mountPath, const std::string& user, const std::string& password);

} // namespace fujinet::platform::esp32