#pragma once

namespace fujinet::platform::esp32 {

// Mounts the internal flash filesystem (LittleFS) at /fujifs.
// Returns true on success, false on failure.
bool init_littlefs();

} // namespace fujinet::platform::esp32
