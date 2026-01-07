#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "fujinet/disk/disk_types.h"
#include "fujinet/fs/filesystem.h"

namespace fujinet::disk {

// Per-format image handler (ATR/SSD/DSD/etc).
// Pure core: no bus/protocol concerns.
class IDiskImage {
public:
    virtual ~IDiskImage() = default;

    virtual ImageType type() const noexcept = 0;
    virtual DiskGeometry geometry() const noexcept = 0;
    virtual bool read_only() const noexcept = 0;

    // Take ownership of an already-open file plus its size.
    // Implementations should parse headers and establish geometry here.
    virtual DiskResult mount(
        std::unique_ptr<fs::IFile> file,
        std::uint64_t sizeBytes,
        const MountOptions& opts
    ) = 0;

    virtual DiskResult unmount() = 0;

    // Sector I/O is LBA-based.
    // Caller provides a buffer at least geometry().sectorSize bytes.
    virtual DiskResult read_sector(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) = 0;
    virtual DiskResult write_sector(std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes) = 0;

    virtual DiskResult flush() = 0;
};

} // namespace fujinet::disk


