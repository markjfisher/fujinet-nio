#include "fujinet/disk/d64_image.h"

#include "fujinet/disk/raw_image.h"

#include <utility>

namespace fujinet::disk {

namespace {

constexpr std::uint16_t sector_bytes = 256;
constexpr std::uint32_t sectors_35_tracks = 683;
constexpr std::uint32_t sectors_40_tracks = 768;
constexpr std::uint32_t sectors_42_tracks = 802;

class D64DiskImage final : public IDiskImage {
public:
    ImageType type() const noexcept override { return ImageType::D64; }
    DiskGeometry geometry() const noexcept override { return _sectors->geometry(); }
    bool read_only() const noexcept override { return _sectors->read_only(); }

    DiskResult mount(
        std::unique_ptr<fs::IFile> file,
        std::uint64_t sizeBytes,
        const MountOptions& opts
    ) override
    {
        DiskGeometry geometry{};
        if (!d64_geometry_for_size(sizeBytes, geometry)) {
            return DiskResult{DiskError::BadImage};
        }
        MountOptions sectorOpts = opts;
        sectorOpts.sectorSizeHint = 0;
        sectorOpts.geometryHint = geometry;
        // The flat image sees only the sector area; an error table after it is
        // never read or written.
        const std::uint64_t sectorBytes =
            static_cast<std::uint64_t>(geometry.sectorSize) * geometry.sectorCount;
        return _sectors->mount(std::move(file), sectorBytes, sectorOpts);
    }

    DiskResult unmount() override { return _sectors->unmount(); }

    DiskResult read_sector(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) override
    {
        return _sectors->read_sector(lba, dst, dstBytes);
    }

    DiskResult write_sector(std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes) override
    {
        return _sectors->write_sector(lba, src, srcBytes);
    }

    DiskResult flush() override { return _sectors->flush(); }

    DiskImageStats image_stats() const noexcept override { return _sectors->image_stats(); }
    void reset_image_stats() noexcept override { _sectors->reset_image_stats(); }

private:
    std::unique_ptr<IDiskImage> _sectors{make_raw_disk_image()};
};

} // namespace

bool d64_geometry_for_size(std::uint64_t sizeBytes, DiskGeometry& out) noexcept
{
    std::uint32_t sectors = 0;
    switch (sizeBytes) {
        case sectors_35_tracks * sector_bytes:
        case sectors_35_tracks * sector_bytes + sectors_35_tracks:
            sectors = sectors_35_tracks;
            break;
        case sectors_40_tracks * sector_bytes:
        case sectors_40_tracks * sector_bytes + sectors_40_tracks:
            sectors = sectors_40_tracks;
            break;
        case sectors_42_tracks * sector_bytes:
        case sectors_42_tracks * sector_bytes + sectors_42_tracks:
            sectors = sectors_42_tracks;
            break;
        default:
            return false;
    }
    out.sectorSize = sector_bytes;
    out.sectorCount = sectors;
    out.supportsVariableSectorSize = false;
    return true;
}

std::unique_ptr<IDiskImage> make_d64_disk_image()
{
    return std::make_unique<D64DiskImage>();
}

} // namespace fujinet::disk
