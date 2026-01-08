#include "fujinet/disk/atr_image.h"

#include <array>
#include <cstring>

namespace fujinet::disk {

namespace {
static constexpr std::uint16_t ATR_MAGIC = 0x0296;
static constexpr std::uint64_t ATR_HEADER_BYTES = 16;

static std::uint16_t u16le(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

static std::uint32_t sector_size_for(std::uint16_t baseSectorSize, std::uint32_t sector1based) noexcept
{
    // ATR convention: if base sector size is 256, the first three sectors are always 128 bytes.
    if (baseSectorSize == 256 && sector1based <= 3) return 128;
    return baseSectorSize;
}

static std::uint64_t sector_to_offset(std::uint16_t baseSectorSize, std::uint32_t sector1based) noexcept
{
    // Derived from classic ATR mapping:
    // - header is 16 bytes
    // - first three sectors may be 128 bytes when baseSectorSize==256
    if (sector1based == 1) {
        return ATR_HEADER_BYTES;
    }
    if (sector1based == 2) {
        return ATR_HEADER_BYTES + ((baseSectorSize == 512) ? 512 : 128);
    }
    if (sector1based == 3) {
        if (baseSectorSize == 512) {
            return ATR_HEADER_BYTES + 1024;
        }
        return ATR_HEADER_BYTES + 256;
    }

    if (baseSectorSize == 256) {
        // Sectors 1..3 are 128, then 256 thereafter.
        // Data offset after sectors 1..3 is 16 + 3*128.
        return ATR_HEADER_BYTES + (3ull * 128ull) + static_cast<std::uint64_t>(sector1based - 4) * 256ull;
    }

    // baseSectorSize is 128 or 512 typically (others are rare)
    return ATR_HEADER_BYTES + static_cast<std::uint64_t>(sector1based - 1) * static_cast<std::uint64_t>(baseSectorSize);
}
} // namespace


class AtrDiskImage final : public IDiskImage {
public:
    ImageType type() const noexcept override { return ImageType::Atr; }
    DiskGeometry geometry() const noexcept override { return _geo; }
    bool read_only() const noexcept override { return _readOnly; }

    DiskResult mount(
        std::unique_ptr<fs::IFile> file,
        std::uint64_t sizeBytes,
        const MountOptions& opts
    ) override
    {
        if (!file) return DiskResult{DiskError::OpenFailed};
        if (sizeBytes < ATR_HEADER_BYTES) return DiskResult{DiskError::BadImage};

        if (!file->seek(0)) return DiskResult{DiskError::IoError};

        std::array<std::uint8_t, 16> hdr{};
        const std::size_t got = file->read(hdr.data(), hdr.size());
        if (got != hdr.size()) return DiskResult{DiskError::IoError};

        const std::uint16_t magic = u16le(&hdr[0]);
        if (magic != ATR_MAGIC) return DiskResult{DiskError::BadImage};

        const std::uint32_t paragraphs =
            static_cast<std::uint32_t>(u16le(&hdr[2])) | (static_cast<std::uint32_t>(hdr[6]) << 16);
        const std::uint16_t baseSectorSize = u16le(&hdr[4]);
        if (!(baseSectorSize == 128 || baseSectorSize == 256 || baseSectorSize == 512)) {
            return DiskResult{DiskError::BadImage};
        }

        const std::uint64_t dataBytes = static_cast<std::uint64_t>(paragraphs) * 16ull;
        if (dataBytes + ATR_HEADER_BYTES > sizeBytes) {
            // Header claims more data than file contains
            return DiskResult{DiskError::BadImage};
        }

        std::uint32_t sectorCount = static_cast<std::uint32_t>(dataBytes / baseSectorSize);
        if (baseSectorSize == 256) {
            // First three sectors are 128, so the logical sector count increases by 2.
            sectorCount += 2;
        }
        if (sectorCount == 0) return DiskResult{DiskError::BadImage};

        _file = std::move(file);
        _readOnly = opts.readOnlyRequested;
        _baseSectorSize = baseSectorSize;
        _geo.sectorSize = baseSectorSize; // maximum size in this format
        _geo.sectorCount = sectorCount;
        _geo.supportsVariableSectorSize = (baseSectorSize == 256);

        return DiskResult{DiskError::None};
    }

    DiskResult unmount() override
    {
        _file.reset();
        _geo = {};
        _readOnly = true;
        _baseSectorSize = 0;
        return DiskResult{DiskError::None};
    }

    DiskResult read_sector(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (!dst) return DiskResult{DiskError::InvalidSlot};
        if (_geo.sectorCount == 0) return DiskResult{DiskError::BadImage};
        if (lba >= _geo.sectorCount) return DiskResult{DiskError::OutOfRange};
        if (dstBytes < _geo.sectorSize) return DiskResult{DiskError::InvalidSlot};

        const std::uint32_t sector1 = lba + 1;
        const std::uint32_t secSize = sector_size_for(_baseSectorSize, sector1);
        const std::uint64_t off = sector_to_offset(_baseSectorSize, sector1);

        if (!_file->seek(off)) return DiskResult{DiskError::IoError};

        // Zero-fill the destination up to max size, then read actual bytes.
        std::memset(dst, 0, _geo.sectorSize);
        const std::size_t got = _file->read(dst, secSize);
        if (got != secSize) return DiskResult{DiskError::IoError};

        return DiskResult{DiskError::None, static_cast<std::uint16_t>(secSize)};
    }

    DiskResult write_sector(std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes) override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (_readOnly) return DiskResult{DiskError::ReadOnly};
        if (!src) return DiskResult{DiskError::InvalidSlot};
        if (_geo.sectorCount == 0) return DiskResult{DiskError::BadImage};
        if (lba >= _geo.sectorCount) return DiskResult{DiskError::OutOfRange};

        const std::uint32_t sector1 = lba + 1;
        const std::uint32_t secSize = sector_size_for(_baseSectorSize, sector1);
        if (srcBytes < secSize) return DiskResult{DiskError::InvalidRequest};

        const std::uint64_t off = sector_to_offset(_baseSectorSize, sector1);
        if (!_file->seek(off)) return DiskResult{DiskError::IoError};

        const std::size_t wrote = _file->write(src, secSize);
        if (wrote != secSize) return DiskResult{DiskError::IoError};

        return DiskResult{DiskError::None, static_cast<std::uint16_t>(secSize)};
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
    std::uint16_t _baseSectorSize{0};
};

std::unique_ptr<IDiskImage> make_atr_disk_image()
{
    return std::make_unique<AtrDiskImage>();
}

} // namespace fujinet::disk


