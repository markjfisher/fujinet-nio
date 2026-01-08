#pragma once

#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// Flat sector image (no header). Primarily for tests/tools in v1.
std::unique_ptr<IDiskImage> make_raw_disk_image();

// Create a blank raw image into an already-open file (expected truncate/opened "wb").
DiskResult create_raw_image_file(fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount);

} // namespace fujinet::disk


