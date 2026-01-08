#include "fujinet/disk/raw_image.h"

namespace fujinet::disk {

class RawDiskImage final : public IDiskImage {
public:
    ImageType type() const noexcept override { return ImageType::Raw; }
    DiskGeometry geometry() const noexcept override { return _geo; }
    bool read_only() const noexcept override { return _readOnly; }

    DiskResult mount(
        std::unique_ptr<fs::IFile> file,
        std::uint64_t sizeBytes,
        const MountOptions& opts
    ) override
    {
        if (!file) return DiskResult{DiskError::OpenFailed};

        const std::uint16_t sectorSize = opts.sectorSizeHint ? opts.sectorSizeHint : 256;
        if (sectorSize == 0) return DiskResult{DiskError::BadImage};
        if ((sizeBytes % sectorSize) != 0) return DiskResult{DiskError::BadImage};

        _file = std::move(file);
        _readOnly = opts.readOnlyRequested;
        _geo.sectorSize = sectorSize;
        _geo.sectorCount = static_cast<std::uint32_t>(sizeBytes / sectorSize);
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

std::unique_ptr<IDiskImage> make_raw_disk_image()
{
    return std::make_unique<RawDiskImage>();
}

} // namespace fujinet::disk


