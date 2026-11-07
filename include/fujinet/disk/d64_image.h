#pragma once

#include <cstdint>
#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// Commodore 1541 disk image: flat 256-byte sectors in track/sector order, no
// header. Sector I/O only; the DOS structures on the disk are read by
// CbmVolume (cbm_volume.h).
std::unique_ptr<IDiskImage> make_d64_disk_image();

// The file sizes in use: 35, 40 or 42 tracks, each with or without the optional
// one-byte-per-sector error table at the end of the file. The error table is not
// exposed; every sector reads as stored.
bool d64_geometry_for_size(std::uint64_t sizeBytes, DiskGeometry& out) noexcept;

} // namespace fujinet::disk
