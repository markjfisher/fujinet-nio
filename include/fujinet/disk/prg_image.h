#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// A loose Commodore program (.prg) presented as a synthesised, read-only, 35-track
// 1541 disk. The file is the disk's only directory entry, named after the source
// file, so both LOAD"*",8,1 and LOAD"NAME",8 find it. Sector contents are computed
// per read from the source file; only the file handle is kept. Written from the
// public 1541 disk format. See docs/disk_device_protocol.md.
std::unique_ptr<IDiskImage> make_prg_disk_image();

// Geometry of that disk. False for a file under two bytes (no load address) or one
// bigger than a 35-track disk holds (664 data sectors of 254 bytes).
bool prg_disk_geometry(std::uint64_t sizeBytes, DiskGeometry& out) noexcept;

// The name a path gives the file on the disk: the last path element without a .prg
// extension, in PETSCII capitals, at most 16 bytes. Bytes that mean something in a
// CBM file name (wildcards, separators, a leading $) become '-', so the file can
// always be asked for by name.
std::string prg_disk_name(std::string_view path);

} // namespace fujinet::disk
