#include "fujinet/disk/image_probers/image_probe.h"

#include "fujinet/disk/image_probers/fat_bpb_probe.h"
#include "fujinet/io/devices/byte_codec.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace fujinet::disk {

namespace {

using fujinet::io::bytecodec::read_u16le;

static constexpr std::uint16_t ATR_MAGIC = 0x0296;
static constexpr std::uint64_t ATR_HEADER_BYTES = 16;
static constexpr std::uint64_t SSD_HEADER_SECTOR_COUNT_OFF_HI = 0x106;
static constexpr std::uint64_t SSD_HEADER_SECTOR_COUNT_OFF_LO = 0x107;
static constexpr std::uint64_t SSD_HEADER_MIN_BYTES = 0x108;

static std::string lower_ascii(std::string_view s)
{
    std::string out(s);
    for (auto& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

static std::string extension_of(std::string_view path)
{
    const std::string p = lower_ascii(path);
    const auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return {};
    return p.substr(dot + 1);
}

static bool is_raw_extension(std::string_view ext) noexcept
{
    return ext == "img" || ext == "ima" || ext == "raw";
}

class AtrHeaderProbe final : public IImageProbe {
public:
    ImageProbeResult probe(
        fs::IFile& file,
        std::uint64_t sizeBytes,
        std::string_view,
        const MountOptions&
    ) const override
    {
        if (sizeBytes < ATR_HEADER_BYTES || !file.seek(0)) return {};

        std::uint8_t hdr[ATR_HEADER_BYTES]{};
        if (file.read(hdr, sizeof(hdr)) != sizeof(hdr)) return {};
        if (read_u16le(&hdr[0]) != ATR_MAGIC) return {};

        const std::uint32_t paragraphs =
            static_cast<std::uint32_t>(read_u16le(&hdr[2])) | (static_cast<std::uint32_t>(hdr[6]) << 16);
        const std::uint16_t baseSectorSize = read_u16le(&hdr[4]);
        if (!(baseSectorSize == 128 || baseSectorSize == 256 || baseSectorSize == 512)) return {};

        const std::uint64_t dataBytes = static_cast<std::uint64_t>(paragraphs) * 16ull;
        if (dataBytes + ATR_HEADER_BYTES > sizeBytes) return {};

        std::uint32_t sectorCount = static_cast<std::uint32_t>(dataBytes / baseSectorSize);
        if (baseSectorSize == 256) sectorCount += 2;
        if (sectorCount == 0) return {};

        DiskGeometry geometry{};
        geometry.sectorSize = baseSectorSize;
        geometry.sectorCount = sectorCount;
        geometry.supportsVariableSectorSize = (baseSectorSize == 256);
        return {true, ImageType::Atr, geometry, ImageProbeConfidence::Content};
    }
};

class SsdDfsProbe final : public IImageProbe {
public:
    ImageProbeResult probe(
        fs::IFile& file,
        std::uint64_t sizeBytes,
        std::string_view path,
        const MountOptions&
    ) const override
    {
        if (extension_of(path) != "ssd") return {};
        if (sizeBytes < SSD_HEADER_MIN_BYTES || !file.seek(0)) return {};

        std::uint8_t header[SSD_HEADER_MIN_BYTES]{};
        if (file.read(header, sizeof(header)) != sizeof(header)) return {};

        const std::uint32_t sectors =
            (static_cast<std::uint32_t>(header[SSD_HEADER_SECTOR_COUNT_OFF_HI] & 0x03u) << 8)
            | static_cast<std::uint32_t>(header[SSD_HEADER_SECTOR_COUNT_OFF_LO]);
        if (!(sectors == 400 || sectors == 800)) return {};

        DiskGeometry geometry{};
        geometry.sectorSize = 256;
        geometry.sectorCount = sectors;
        geometry.supportsVariableSectorSize = false;
        return {true, ImageType::Ssd, geometry, ImageProbeConfidence::Content};
    }
};

class ExtensionProbe final : public IImageProbe {
public:
    ImageProbeResult probe(
        fs::IFile&,
        std::uint64_t sizeBytes,
        std::string_view path,
        const MountOptions& opts
    ) const override
    {
        const std::string ext = extension_of(path);
        if (ext == "atr") return {true, ImageType::Atr, {}, ImageProbeConfidence::Extension};
        if (ext == "ssd") return {true, ImageType::Ssd, {}, ImageProbeConfidence::Extension};
        if (ext == "dsd") return {true, ImageType::Dsd, {}, ImageProbeConfidence::Extension};
        if (is_raw_extension(ext)) {
            DiskGeometry geometry{};
            if (opts.sectorSizeHint != 0 && (sizeBytes % opts.sectorSizeHint) == 0) {
                geometry.sectorSize = opts.sectorSizeHint;
                geometry.sectorCount = static_cast<std::uint32_t>(sizeBytes / opts.sectorSizeHint);
            }
            return {true, ImageType::Raw, geometry, opts.sectorSizeHint ? ImageProbeConfidence::Hint : ImageProbeConfidence::Extension};
        }
        if (opts.sectorSizeHint != 0 && (sizeBytes % opts.sectorSizeHint) == 0) {
            DiskGeometry geometry{};
            geometry.sectorSize = opts.sectorSizeHint;
            geometry.sectorCount = static_cast<std::uint32_t>(sizeBytes / opts.sectorSizeHint);
            return {true, ImageType::Raw, geometry, ImageProbeConfidence::Hint};
        }
        return {};
    }
};

} // namespace

bool has_geometry(const DiskGeometry& geometry) noexcept
{
    return geometry.sectorSize != 0 && geometry.sectorCount != 0;
}

bool ProbeRegistry::registerProbe(std::unique_ptr<IImageProbe> probe)
{
    if (!probe) return false;
    _probes.push_back(std::move(probe));
    return true;
}

ImageProbeResult ProbeRegistry::probe(
    fs::IFile& file,
    std::uint64_t sizeBytes,
    std::string_view path,
    const MountOptions& opts
) const
{
    for (const auto& probe : _probes) {
        const auto result = probe->probe(file, sizeBytes, path, opts);
        if (result.matched && result.type != ImageType::Auto) {
            return result;
        }
    }
    return {};
}

ProbeRegistry make_default_probe_registry()
{
    ProbeRegistry registry;
    registry.registerProbe(std::make_unique<AtrHeaderProbe>());
    registry.registerProbe(std::make_unique<FatBpbSectorSizeProbe>());
    registry.registerProbe(std::make_unique<SsdDfsProbe>());
    registry.registerProbe(std::make_unique<ExtensionProbe>());
    return registry;
}

ImageProbeResult probe_image(
    fs::IFile& file,
    std::uint64_t sizeBytes,
    std::string_view path,
    const MountOptions& opts
)
{
    static const ProbeRegistry registry = make_default_probe_registry();
    return registry.probe(file, sizeBytes, path, opts);
}

} // namespace fujinet::disk
