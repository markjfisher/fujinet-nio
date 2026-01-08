#pragma once

#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// BBC DFS SSD image: flat 256-byte sectors with fixed known sizes (40 or 80 track).
// v1 scope: geometry + sector read/write only (no catalog parsing).
std::unique_ptr<IDiskImage> make_ssd_disk_image();

// Create a blank SSD image into an already-open file (expected truncate/opened "wb").
DiskResult create_ssd_image_file(fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount);

} // namespace fujinet::disk


