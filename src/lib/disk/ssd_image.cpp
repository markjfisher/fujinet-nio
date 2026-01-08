#include "fujinet/disk/ssd_image.h"

#include <cstring>

namespace fujinet::disk {

class SsdDiskImage final : public IDiskImage {
public:
    ImageType type() const noexcept override { return ImageType::Ssd; }
    DiskGeometry geometry() const noexcept override { return _geo; }
    bool read_only() const noexcept override { return _readOnly; }

    DiskResult mount(
        std::unique_ptr<fs::IFile> file,
        std::uint64_t sizeBytes,
        const MountOptions& opts
    ) override
    {
        if (!file) return DiskResult{DiskError::OpenFailed};

        // SSD is a flat 256-byte sector image. Common sizes:
        // - 40 track:  400 sectors = 102,400 bytes
        // - 80 track:  800 sectors = 204,800 bytes
        static constexpr std::uint16_t SECTOR_SIZE = 256;

        if ((sizeBytes % SECTOR_SIZE) != 0) return DiskResult{DiskError::BadImage};

        const std::uint32_t sectors = static_cast<std::uint32_t>(sizeBytes / SECTOR_SIZE);
        if (!(sectors == 400 || sectors == 800)) {
            return DiskResult{DiskError::BadImage};
        }

        _file = std::move(file);
        _readOnly = opts.readOnlyRequested;

        _geo.sectorSize = SECTOR_SIZE;
        _geo.sectorCount = sectors;
        _geo.supportsVariableSectorSize = false;

        return DiskResult{DiskError::None};
    }

    DiskResult unmount() override
    {
        _file.reset();
        _geo = {};
        _readOnly = true;
        return DiskResult{DiskError::None};
    }

    DiskResult read_sector(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (!dst) return DiskResult{DiskError::InvalidSlot};
        if (_geo.sectorSize == 0 || _geo.sectorCount == 0) return DiskResult{DiskError::BadImage};
        if (lba >= _geo.sectorCount) return DiskResult{DiskError::OutOfRange};
        if (dstBytes < _geo.sectorSize) return DiskResult{DiskError::InvalidSlot};

        const std::uint64_t off = static_cast<std::uint64_t>(lba) * _geo.sectorSize;
        if (!_file->seek(off)) return DiskResult{DiskError::IoError};
        const std::size_t got = _file->read(dst, _geo.sectorSize);
        if (got != _geo.sectorSize) return DiskResult{DiskError::IoError};
        return DiskResult{DiskError::None, static_cast<std::uint16_t>(_geo.sectorSize)};
    }

    DiskResult write_sector(std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes) override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (_readOnly) return DiskResult{DiskError::ReadOnly};
        if (!src) return DiskResult{DiskError::InvalidSlot};
        if (_geo.sectorSize == 0 || _geo.sectorCount == 0) return DiskResult{DiskError::BadImage};
        if (lba >= _geo.sectorCount) return DiskResult{DiskError::OutOfRange};
        if (srcBytes < _geo.sectorSize) return DiskResult{DiskError::InvalidSlot};

        const std::uint64_t off = static_cast<std::uint64_t>(lba) * _geo.sectorSize;
        if (!_file->seek(off)) return DiskResult{DiskError::IoError};
        const std::size_t wrote = _file->write(src, _geo.sectorSize);
        if (wrote != _geo.sectorSize) return DiskResult{DiskError::IoError};
        return DiskResult{DiskError::None, static_cast<std::uint16_t>(_geo.sectorSize)};
    }

    DiskResult flush() override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        return DiskResult{_file->flush() ? DiskError::None : DiskError::IoError};
    }

private:
    std::unique_ptr<fs::IFile> _file;
    DiskGeometry _geo{};
    bool _readOnly{true};
};

std::unique_ptr<IDiskImage> make_ssd_disk_image()
{
    return std::make_unique<SsdDiskImage>();
}

DiskResult create_ssd_image_file(fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount)
{
    if (sectorSize != 256) return DiskResult{DiskError::InvalidGeometry};
    if (!(sectorCount == 400 || sectorCount == 800)) return DiskResult{DiskError::InvalidGeometry};

    // Write a minimal DFS "blank" catalog (matches classic firmware behavior):
    // - sector 0: all zeros
    // - sector 1: disk title, sector count fields
    std::uint8_t sec[256]{};

    // sector 0
    if (file.write(sec, sizeof(sec)) != sizeof(sec)) return DiskResult{DiskError::IoError};

    // sector 1
    std::memset(sec, 0, sizeof(sec));
    std::memcpy(sec, "BLANK   ", 8); // Disk title (8 bytes)
    sec[0xF6] = 0; // No files * 8
    sec[0xF7] = 0; // Boot option
    sec[0xF8] = static_cast<std::uint8_t>(sectorCount & 0xFF);
    sec[0xF9] = static_cast<std::uint8_t>((sectorCount >> 8) & 0xFF);
    if (file.write(sec, sizeof(sec)) != sizeof(sec)) return DiskResult{DiskError::IoError};

    // Ensure final file size (sparse-extend).
    const std::uint64_t total = 256ull * sectorCount;
    if (!file.seek(total - 1)) return DiskResult{DiskError::IoError};
    const std::uint8_t z = 0;
    if (file.write(&z, 1) != 1) return DiskResult{DiskError::IoError};
    return DiskResult{DiskError::None};
}

} // namespace fujinet::disk


