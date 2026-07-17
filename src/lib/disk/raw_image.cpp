#include "fujinet/disk/raw_image.h"

namespace fujinet::disk {

namespace {

static std::uint16_t get_u16le(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}

static bool is_valid_sector_size(std::uint16_t sectorSize) noexcept
{
    return sectorSize == 128 || sectorSize == 256 || sectorSize == 512 ||
           sectorSize == 1024 || sectorSize == 2048 || sectorSize == 4096;
}

static bool is_power_of_two(std::uint8_t value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}

static bool looks_like_fat_bpb(const std::uint8_t* sector, std::uint64_t sizeBytes) noexcept
{
    const bool hasJump = (sector[0] == 0xeb && sector[2] == 0x90) || sector[0] == 0xe9;
    if (!hasJump) return false;
    if (sector[510] != 0x55 || sector[511] != 0xaa) return false;

    const auto sectorSize = get_u16le(&sector[11]);
    if (!is_valid_sector_size(sectorSize)) return false;
    if ((sizeBytes % sectorSize) != 0) return false;

    const auto sectorsPerCluster = sector[13];
    const auto reservedSectors = get_u16le(&sector[14]);
    const auto fatCount = sector[16];
    const auto rootEntryCount = get_u16le(&sector[17]);
    const auto totalSectors16 = get_u16le(&sector[19]);
    const auto sectorsPerFat16 = get_u16le(&sector[22]);

    if (!is_power_of_two(sectorsPerCluster)) return false;
    if (reservedSectors == 0 || fatCount == 0 || sectorsPerFat16 == 0) return false;
    if (rootEntryCount == 0) return false;

    const std::uint32_t totalSectors32 =
        static_cast<std::uint32_t>(sector[32]) |
        (static_cast<std::uint32_t>(sector[33]) << 8) |
        (static_cast<std::uint32_t>(sector[34]) << 16) |
        (static_cast<std::uint32_t>(sector[35]) << 24);
    const std::uint32_t totalSectors = totalSectors16 ? totalSectors16 : totalSectors32;
    if (totalSectors == 0) return false;

    return static_cast<std::uint64_t>(totalSectors) * sectorSize == sizeBytes;
}

static std::uint16_t infer_raw_sector_size(fs::IFile& file, std::uint64_t sizeBytes)
{
    if (sizeBytes >= 512 && file.seek(0)) {
        std::uint8_t sector[512]{};
        if (file.read(sector, sizeof(sector)) == sizeof(sector) && looks_like_fat_bpb(sector, sizeBytes)) {
            return get_u16le(&sector[11]);
        }
    }

    return 256;
}

} // namespace

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

        const std::uint16_t sectorSize = opts.sectorSizeHint ? opts.sectorSizeHint : infer_raw_sector_size(*file, sizeBytes);
        if (!is_valid_sector_size(sectorSize)) return DiskResult{DiskError::BadImage};
        if ((sizeBytes % sectorSize) != 0) return DiskResult{DiskError::BadImage};

        _file = std::move(file);
        _readOnly = opts.readOnlyRequested;
        _geo.sectorSize = sectorSize;
        _geo.sectorCount = static_cast<std::uint32_t>(sizeBytes / sectorSize);
        _geo.supportsVariableSectorSize = false;
        _cursorValid = false;
        _nextSequentialLba = 0;
        _stats = {};

        return DiskResult{DiskError::None};
    }

    DiskResult unmount() override
    {
        _file.reset();
        _geo = {};
        _readOnly = true;
        _cursorValid = false;
        _nextSequentialLba = 0;
        _stats = {};
        return DiskResult{DiskError::None};
    }

    DiskResult read_sector(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (!dst) return DiskResult{DiskError::InvalidSlot};
        if (_geo.sectorSize == 0 || _geo.sectorCount == 0) return DiskResult{DiskError::BadImage};
        if (lba >= _geo.sectorCount) return DiskResult{DiskError::OutOfRange};
        if (dstBytes < _geo.sectorSize) return DiskResult{DiskError::InvalidSlot};

        ++_stats.readOps;
        if (!_cursorValid || lba != _nextSequentialLba) {
            const std::uint64_t off = static_cast<std::uint64_t>(lba) * _geo.sectorSize;
            ++_stats.seekOps;
            if (!_file->seek(off)) {
                _cursorValid = false;
                return DiskResult{DiskError::IoError};
            }
        } else {
            ++_stats.sequentialReadHits;
        }
        const std::size_t got = _file->read(dst, _geo.sectorSize);
        if (got != _geo.sectorSize) {
            _cursorValid = false;
            return DiskResult{DiskError::IoError};
        }
        _stats.readBytes += got;
        _cursorValid = true;
        _nextSequentialLba = lba + 1;
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

        ++_stats.writeOps;
        if (!_cursorValid || lba != _nextSequentialLba) {
            const std::uint64_t off = static_cast<std::uint64_t>(lba) * _geo.sectorSize;
            ++_stats.seekOps;
            if (!_file->seek(off)) {
                _cursorValid = false;
                return DiskResult{DiskError::IoError};
            }
        } else {
            ++_stats.sequentialWriteHits;
        }
        const std::size_t wrote = _file->write(src, _geo.sectorSize);
        if (wrote != _geo.sectorSize) {
            _cursorValid = false;
            return DiskResult{DiskError::IoError};
        }
        _stats.writeBytes += wrote;
        _cursorValid = true;
        _nextSequentialLba = lba + 1;
        return DiskResult{DiskError::None, static_cast<std::uint16_t>(_geo.sectorSize)};
    }

    DiskResult flush() override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        return DiskResult{_file->flush() ? DiskError::None : DiskError::IoError};
    }

    DiskImageStats image_stats() const noexcept override { return _stats; }
    void reset_image_stats() noexcept override { _stats = {}; }

private:
    std::unique_ptr<fs::IFile> _file;
    DiskGeometry _geo{};
    bool _readOnly{true};
    bool _cursorValid{false};
    std::uint32_t _nextSequentialLba{0};
    DiskImageStats _stats{};
};

std::unique_ptr<IDiskImage> make_raw_disk_image()
{
    return std::make_unique<RawDiskImage>();
}

DiskResult create_raw_image_file(fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount)
{
    if (sectorSize == 0 || sectorCount == 0) return DiskResult{DiskError::InvalidGeometry};
    const std::uint64_t total = static_cast<std::uint64_t>(sectorSize) * sectorCount;
    if (total == 0) return DiskResult{DiskError::InvalidGeometry};
    if (!file.seek(total - 1)) return DiskResult{DiskError::IoError};
    const std::uint8_t z = 0;
    if (file.write(&z, 1) != 1) return DiskResult{DiskError::IoError};
    return DiskResult{DiskError::None};
}

} // namespace fujinet::disk
