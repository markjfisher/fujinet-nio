#pragma once

namespace fujinet::platform::esp32 {

// Mounts the internal flash filesystem (LittleFS) at /fujifs.
// Returns true on success, false on failure.
bool init_littlefs();

// Mounts the SD card filesystem at /sdcard.
// Returns true on success, false on failure.
bool init_sdcard_spi();

} // namespace fujinet::platform::esp32
