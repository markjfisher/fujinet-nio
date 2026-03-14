#include "fujinet/disk/ssd_image.h"

#include <algorithm>
#include <cstring>

namespace fujinet::disk {

// DFS disc size in header: sector 1 bytes 6–7 = file offsets $106/$107 (262/263).
// High 2 bits at $106, low 8 bits at $107 → 10-bit sector count.
static constexpr std::uint64_t SSD_HEADER_SECTOR_COUNT_OFF_HI = 0x106;
static constexpr std::uint64_t SSD_HEADER_SECTOR_COUNT_OFF_LO = 0x107;
static constexpr std::uint64_t SSD_HEADER_MIN_BYTES = 0x108;

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
        // Sparse images may be truncated; sector count comes from DFS header (bytes 106–107).
        static constexpr std::uint16_t SECTOR_SIZE = 256;

        if (sizeBytes < SSD_HEADER_MIN_BYTES) return DiskResult{DiskError::BadImage};

        std::uint8_t header[SSD_HEADER_MIN_BYTES]{};
        if (!file->seek(0)) return DiskResult{DiskError::IoError};
        if (file->read(header, sizeof(header)) != sizeof(header)) return DiskResult{DiskError::IoError};

        const std::uint32_t sectors = (static_cast<std::uint32_t>(header[SSD_HEADER_SECTOR_COUNT_OFF_HI] & 0x03u) << 8)
            | static_cast<std::uint32_t>(header[SSD_HEADER_SECTOR_COUNT_OFF_LO]);
        if (!(sectors == 400 || sectors == 800)) return DiskResult{DiskError::BadImage};

        _file = std::move(file);
        _readOnly = opts.readOnlyRequested;
        _fileSizeBytes = sizeBytes;

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
        _fileSizeBytes = 0;
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
        const std::uint64_t end = off + _geo.sectorSize;

        if (off >= _fileSizeBytes) {
            std::memset(dst, 0, _geo.sectorSize);
            return DiskResult{DiskError::None, static_cast<std::uint16_t>(_geo.sectorSize)};
        }
        if (end <= _fileSizeBytes) {
            if (!_file->seek(off)) return DiskResult{DiskError::IoError};
            const std::size_t got = _file->read(dst, _geo.sectorSize);
            if (got != _geo.sectorSize) return DiskResult{DiskError::IoError};
            return DiskResult{DiskError::None, static_cast<std::uint16_t>(_geo.sectorSize)};
        }
        std::size_t inFile = static_cast<std::size_t>(_fileSizeBytes - off);
        if (!_file->seek(off)) return DiskResult{DiskError::IoError};
        const std::size_t got = _file->read(dst, inFile);
        if (got != inFile) return DiskResult{DiskError::IoError};
        std::memset(dst + inFile, 0, _geo.sectorSize - inFile);
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
        const std::uint64_t end = off + _geo.sectorSize;

        if (off > _fileSizeBytes) {
            static constexpr std::size_t ZERO_BUF = 256;
            std::uint8_t zeros[ZERO_BUF]{};
            if (!_file->seek(_fileSizeBytes)) return DiskResult{DiskError::IoError};
            for (std::uint64_t pos = _fileSizeBytes; pos < off; ) {
                const std::size_t chunk = static_cast<std::size_t>(std::min(off - pos, static_cast<std::uint64_t>(ZERO_BUF)));
                if (_file->write(zeros, chunk) != chunk) return DiskResult{DiskError::IoError};
                pos += chunk;
            }
            _fileSizeBytes = off;
        }
        if (!_file->seek(off)) return DiskResult{DiskError::IoError};
        const std::size_t wrote = _file->write(src, _geo.sectorSize);
        if (wrote != _geo.sectorSize) return DiskResult{DiskError::IoError};
        if (end > _fileSizeBytes) _fileSizeBytes = end;
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
    std::uint64_t _fileSizeBytes{0};  // actual file size (for sparse read/write)
};

std::unique_ptr<IDiskImage> make_ssd_disk_image()
{
    return std::make_unique<SsdDiskImage>();
}

DiskResult create_ssd_image_file(fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount)
{
    if (sectorSize != 256) return DiskResult{DiskError::InvalidGeometry};
    if (!(sectorCount == 400 || sectorCount == 800)) return DiskResult{DiskError::InvalidGeometry};

    // Write a minimal DFS 0.90 catalogue header (2 sectors).
    // Reference: https://beebwiki.mdfs.net/Acorn_DFS_disc_format
    //
    // Sector 0 bytes 0..7: title (first 8 chars), padded with NULs for DFS 0.90.
    // Sector 1 bytes 0..3: title (last 4 chars)
    // Sector 1 byte 4: cycle (BCD) - start at 0.
    // Sector 1 byte 5: file offset = 8 * file_count (0 for blank).
    // Sector 1 byte 6: bits 0..1 disc size high bits; bits 4..5 boot option (0).
    // Sector 1 byte 7: disc size low 8 bits.
    std::uint8_t sec0[256]{};
    std::uint8_t sec1[256]{};

    // Title "BLANK" (DFS permits up to 12 chars). We keep it short and NUL padded.
    std::memcpy(sec0 + 0, "BLANK", 5);

    // cycle=0, file_count=0 => file offset=0 already.
    // boot option=0 (none)
    const std::uint8_t disc_hi = static_cast<std::uint8_t>((sectorCount >> 8) & 0x03);
    sec1[6] = disc_hi; // other bits clear
    sec1[7] = static_cast<std::uint8_t>(sectorCount & 0xFF);

    if (file.write(sec0, sizeof(sec0)) != sizeof(sec0)) return DiskResult{DiskError::IoError};
    if (file.write(sec1, sizeof(sec1)) != sizeof(sec1)) return DiskResult{DiskError::IoError};

    // Ensure final file size (sparse-extend).
    const std::uint64_t total = 256ull * sectorCount;
    if (!file.seek(total - 1)) return DiskResult{DiskError::IoError};
    const std::uint8_t z = 0;
    if (file.write(&z, 1) != 1) return DiskResult{DiskError::IoError};
    return DiskResult{DiskError::None};
}

} // namespace fujinet::disk


