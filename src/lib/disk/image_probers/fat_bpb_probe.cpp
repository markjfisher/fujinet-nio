#include "fujinet/disk/image_probers/fat_bpb_probe.h"

#include "fujinet/io/devices/byte_codec.h"

namespace fujinet::disk {

namespace {

using fujinet::io::bytecodec::read_u16le;
using fujinet::io::bytecodec::read_u32le;

static bool is_power_of_two(std::uint8_t value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}

static bool is_valid_fat_sector_size(std::uint16_t sectorSize) noexcept
{
    return sectorSize == 128 || sectorSize == 256 || sectorSize == 512 ||
           sectorSize == 1024 || sectorSize == 2048 || sectorSize == 4096;
}

static bool looks_like_fat_bpb(const std::uint8_t* sector, std::uint64_t sizeBytes) noexcept
{
    const bool hasJump = (sector[0] == 0xeb && sector[2] == 0x90) || sector[0] == 0xe9;
    if (!hasJump) return false;
    if (sector[510] != 0x55 || sector[511] != 0xaa) return false;

    const auto sectorSize = read_u16le(&sector[11]);
    if (!is_valid_fat_sector_size(sectorSize)) return false;
    if ((sizeBytes % sectorSize) != 0) return false;

    const auto sectorsPerCluster = sector[13];
    const auto reservedSectors = read_u16le(&sector[14]);
    const auto fatCount = sector[16];
    const auto rootEntryCount = read_u16le(&sector[17]);
    const auto totalSectors16 = read_u16le(&sector[19]);
    const auto sectorsPerFat16 = read_u16le(&sector[22]);

    if (!is_power_of_two(sectorsPerCluster)) return false;
    if (reservedSectors == 0 || fatCount == 0 || sectorsPerFat16 == 0) return false;
    if (rootEntryCount == 0) return false;

    const std::uint32_t totalSectors32 = read_u32le(&sector[32]);
    const std::uint32_t totalSectors = totalSectors16 ? totalSectors16 : totalSectors32;
    if (totalSectors == 0) return false;

    return static_cast<std::uint64_t>(totalSectors) * sectorSize == sizeBytes;
}

} // namespace

ImageProbeResult FatBpbSectorSizeProbe::probe(
    fs::IFile& file,
    std::uint64_t sizeBytes,
    std::string_view,
    const MountOptions&
) const
{
    if (sizeBytes < 512 || !file.seek(0)) {
        return {};
    }

    std::uint8_t sector[512]{};
    if (file.read(sector, sizeof(sector)) != sizeof(sector)) {
        return {};
    }
    if (!looks_like_fat_bpb(sector, sizeBytes)) {
        return {};
    }

    DiskGeometry geometry{};
    geometry.sectorSize = read_u16le(&sector[11]);
    geometry.sectorCount = static_cast<std::uint32_t>(sizeBytes / geometry.sectorSize);
    geometry.supportsVariableSectorSize = false;
    return {true, ImageType::Raw, geometry, ImageProbeConfidence::Content};
}

} // namespace fujinet::disk
