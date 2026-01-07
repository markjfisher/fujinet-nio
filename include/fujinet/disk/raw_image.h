#pragma once

#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// Flat sector image (no header). Primarily for tests/tools in v1.
std::unique_ptr<IDiskImage> make_raw_disk_image();

} // namespace fujinet::disk


