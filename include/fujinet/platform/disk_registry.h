#pragma once

#include "fujinet/disk/image_registry.h"

namespace fujinet::platform {

// Build a default disk image-type registry for the current platform.
// Implemented in platform-specific .cpp files (POSIX / ESP32).
disk::ImageRegistry make_default_disk_image_registry();

} // namespace fujinet::platform


