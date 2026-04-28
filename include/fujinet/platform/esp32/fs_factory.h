#pragma once

#include <memory>

#include "fujinet/config/fuji_config.h"
#include "fujinet/fs/filesystem.h"

namespace fujinet::platform::esp32 {

// Wraps LittleFS (or other flash-backed FS) mounted at /fujifs.
std::unique_ptr<fujinet::fs::IFileSystem> create_flash_filesystem();

// Wraps SD card FS mounted at /sdcard (or similar).
// Returns nullptr if we decide SD isn’t available / not mounted.
std::unique_ptr<fujinet::fs::IFileSystem> create_sdcard_filesystem();

// Creates a TNFS filesystem provider. Endpoint is resolved from tnfs:// URI at access time.
// If useTcp is true, TCP transport is forced unless URI explicitly selects UDP.
std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem(bool useTcp = false);

// Creates an HTTP/HTTPS filesystem provider. URLs are resolved at access time.
std::unique_ptr<fujinet::fs::IFileSystem> create_http_filesystem(const fujinet::config::TlsConfig& tlsConfig = {});

} // namespace fujinet::platform::esp32
