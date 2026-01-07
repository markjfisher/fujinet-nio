#pragma once

#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// BBC DFS SSD image: flat 256-byte sectors with fixed known sizes (40 or 80 track).
// v1 scope: geometry + sector read/write only (no catalog parsing).
std::unique_ptr<IDiskImage> make_ssd_disk_image();

} // namespace fujinet::disk


