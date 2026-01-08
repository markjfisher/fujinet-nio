#pragma once

#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// Atari ATR disk image.
// v1 scope: header parsing + geometry + sector read/write only.
std::unique_ptr<IDiskImage> make_atr_disk_image();

// Create a blank ATR image into an already-open file (expected truncate/opened "wb").
DiskResult create_atr_image_file(fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount);

} // namespace fujinet::disk


