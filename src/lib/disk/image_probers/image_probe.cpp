#include "fujinet/disk/image_probers/image_probe.h"

#include "fujinet/disk/d64_image.h"
#include "fujinet/disk/dc42_image.h"
#include "fujinet/disk/image_probers/fat_bpb_probe.h"
#include "fujinet/disk/prg_image.h"
#include "fujinet/disk/xex_boot_image.h"
#include "fujinet/io/devices/byte_codec.h"

#include <algorithm>
#include <cctype>
#include <cstring>
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

static bool is_disk_image_extension(std::string_view ext) noexcept
{
    return ext == "atr" || ext == "xfd" || ext == "ssd" || ext == "dsd" || ext == "adf" ||
           ext == "adl" || ext == "img" || ext == "ima" || ext == "raw" || ext == "dsk" ||
           ext == "hda" || ext == "hfv" || ext == "dc42" || ext == "d64" || ext == "prg";
}

static bool is_adf_extension(std::string_view ext) noexcept
{
    return ext == "adf";
}

// Macintosh hard disk volumes (HD20, SCSI): flat 512-byte blocks. Only
// extensions that mean the same thing on every machine may imply geometry;
// ".dsk" does not (Apple II, Mac, CPC, MSX, TRS-80), so it is left to content
// probes and client hints. See "Adding a new image format" in
// docs/disk_device_protocol.md.
static bool is_mac_volume_extension(std::string_view ext) noexcept
{
    return ext == "hda" || ext == "hfv";
}

// Atari XFD: headerless sectors. The extension is Atari-only, and each
// standard size means one density, so it may imply geometry. Other sizes,
// including double-density images with 128-byte boot sectors (183,936 bytes,
// which a flat raw image cannot express; use ATR), need the client's hint.
static bool xfd_standard_geometry(std::uint64_t sizeBytes, DiskGeometry& out) noexcept
{
    switch (sizeBytes) {
        case 720u * 128u:  out.sectorSize = 128; out.sectorCount = 720;  return true; // single
        case 1040u * 128u: out.sectorSize = 128; out.sectorCount = 1040; return true; // enhanced
        case 720u * 256u:  out.sectorSize = 256; out.sectorCount = 720;  return true; // double
        default: return false;
    }
}

// Geometry from the client's sector size hint, when it fits the file. No
// match otherwise, so the mount reports InvalidGeometry for a hint that
// doesn't fit, or GeometryRequired when there is none.
static ImageProbeResult hinted_raw(std::uint64_t sizeBytes, std::uint16_t hint) noexcept
{
    if (hint == 0 || sizeBytes == 0 || (sizeBytes % hint) != 0) return {};
    DiskGeometry geometry{};
    geometry.sectorSize = hint;
    geometry.sectorCount = static_cast<std::uint32_t>(sizeBytes / hint);
    return {true, ImageType::Raw, geometry, ImageProbeConfidence::Hint};
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

// DiskCopy 4.2 is found by its header, whatever the extension.
class DiskCopy42Probe final : public IImageProbe {
public:
    ImageProbeResult probe(
        fs::IFile& file,
        std::uint64_t sizeBytes,
        std::string_view,
        const MountOptions&
    ) const override
    {
        std::uint8_t header[0x54]{};
        if (sizeBytes < sizeof(header) || !file.seek(0)) return {};
        if (file.read(header, sizeof(header)) != sizeof(header)) return {};
        Dc42Header h;
        if (!parse_dc42_header(header, sizeBytes, h)) return {};

        DiskGeometry geometry{};
        geometry.sectorSize = 512;
        geometry.sectorCount = h.dataBytes / 512;
        return {true, ImageType::DiskCopy42, geometry, ImageProbeConfidence::Content};
    }
};

// Acorn ADFS, old map: the BBC's S, M and L floppies (160K, 320K, 640K) and
// old-map hard discs. Sectors are 256 bytes in ADFS logical order (an .adl's
// sides are already interleaved that way), so it is a flat raw image.
// Recognised by content, whatever the extension: the root directory's "Hugo"
// start and end markers, and the free-space map's disc size matching the
// file. The map checksums are not used; real images often carry stale ones.
class AdfsOldMapProbe final : public IImageProbe {
public:
    ImageProbeResult probe(
        fs::IFile& file,
        std::uint64_t sizeBytes,
        std::string_view,
        const MountOptions&
    ) const override
    {
        static constexpr std::uint64_t DISC_SIZE_OFFSET = 0xFC;   // map sector 0, 24-bit LE
        static constexpr std::uint64_t ROOT_START_MARKER = 0x201; // root dir at sector 2
        static constexpr std::uint64_t ROOT_END_MARKER = 0x6FB;   // 5-sector directory
        if (sizeBytes < ROOT_END_MARKER + 4 || (sizeBytes % 256) != 0) return {};

        std::uint8_t size[3]{};
        std::uint8_t start[4]{};
        std::uint8_t end[4]{};
        if (!file.seek(DISC_SIZE_OFFSET) || file.read(size, sizeof(size)) != sizeof(size)) return {};
        if (!file.seek(ROOT_START_MARKER) || file.read(start, sizeof(start)) != sizeof(start)) return {};
        if (!file.seek(ROOT_END_MARKER) || file.read(end, sizeof(end)) != sizeof(end)) return {};
        if (std::memcmp(start, "Hugo", 4) != 0 || std::memcmp(end, "Hugo", 4) != 0) return {};

        const std::uint32_t sectors = static_cast<std::uint32_t>(size[0]) |
                                      (static_cast<std::uint32_t>(size[1]) << 8) |
                                      (static_cast<std::uint32_t>(size[2]) << 16);
        if (static_cast<std::uint64_t>(sectors) * 256 != sizeBytes) return {};

        DiskGeometry geometry{};
        geometry.sectorSize = 256;
        geometry.sectorCount = sectors;
        return {true, ImageType::Raw, geometry, ImageProbeConfidence::Content};
    }
};

// Atari executables (.xex/.com/.exe, or any file starting with the FF FF
// binary-load header) mount as a synthesised read-only boot disk. By content
// the header alone is weak evidence (a blank 0xFF-filled floppy image starts
// with it), so a header-only match is skipped for extensions that name a disk
// image. A listed executable extension matches without the header, so the
// mount reports BadImage for a file that cannot be loaded.
class XexBootProbe final : public IImageProbe {
public:
    ImageProbeResult probe(
        fs::IFile& file,
        std::uint64_t sizeBytes,
        std::string_view path,
        const MountOptions&
    ) const override
    {
        const std::string ext = extension_of(path);
        const bool exeExt = ext == "xex" || ext == "com" || ext == "exe";
        if (!exeExt && is_disk_image_extension(ext)) return {};

        std::uint8_t head[2]{};
        const bool header = sizeBytes >= 2 && file.seek(0) && file.read(head, 2) == 2 &&
                            xex_has_binary_header(head, 2);
        if (!exeExt && !header) return {};

        DiskGeometry geometry{};
        if (!xex_boot_geometry(sizeBytes, geometry)) return {};
        return {true, ImageType::XexBoot, geometry,
                header ? ImageProbeConfidence::Content : ImageProbeConfidence::Extension};
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
        if (ext == "d64") {
            // The extension is Commodore-only, and a file of another size is
            // left for the mount to refuse as BadImage.
            DiskGeometry geometry{};
            d64_geometry_for_size(sizeBytes, geometry);
            return {true, ImageType::D64, geometry, ImageProbeConfidence::Extension};
        }
        if (ext == "prg") {
            // A loose Commodore program. Other machines' .prg files are programs
            // too, never disk images, so claiming it cannot hide a disk. A file
            // no one-file disk can hold is left for the mount to refuse as BadImage.
            DiskGeometry geometry{};
            prg_disk_geometry(sizeBytes, geometry);
            return {true, ImageType::Prg, geometry, ImageProbeConfidence::Extension};
        }
        if (is_adf_extension(ext)) {
            // ADF is a flat Amiga block image.  Keep it generic/raw, but do
            // not let the raw handler's 256-byte fallback hide malformed
            // ADF sizes.
            const auto sectorCount = sizeBytes / 512;
            if (sizeBytes == 0 || (sizeBytes % 512) != 0 ||
                (sectorCount != 1760 && sectorCount != 3520)) {
                DiskGeometry malformed{};
                malformed.sectorSize = 512;
                return {true, ImageType::Raw, malformed, ImageProbeConfidence::Extension};
            }
            DiskGeometry geometry{};
            geometry.sectorSize = 512;
            geometry.sectorCount = static_cast<std::uint32_t>(sectorCount);
            return {true, ImageType::Raw, geometry, ImageProbeConfidence::Extension};
        }
        // Extensions that imply geometry for a format whose sector size can
        // vary yield to a client hint: a file may have a standard size but
        // another layout.
        if (is_mac_volume_extension(ext)) {
            if (opts.sectorSizeHint) return hinted_raw(sizeBytes, opts.sectorSizeHint);
            if (sizeBytes == 0 || (sizeBytes % 512) != 0) return {};
            DiskGeometry geometry{};
            geometry.sectorSize = 512;
            geometry.sectorCount = static_cast<std::uint32_t>(sizeBytes / 512);
            return {true, ImageType::Raw, geometry, ImageProbeConfidence::Extension};
        }
        if (ext == "xfd") {
            if (opts.sectorSizeHint) return hinted_raw(sizeBytes, opts.sectorSizeHint);
            DiskGeometry geometry{};
            if (!xfd_standard_geometry(sizeBytes, geometry)) return {};
            return {true, ImageType::Raw, geometry, ImageProbeConfidence::Extension};
        }
        if (is_raw_extension(ext)) {
            // Ambiguous: only the client's hint can give geometry. Without a
            // usable one, don't match, so the mount reports GeometryRequired
            // (or InvalidGeometry for a hint that doesn't fit the file).
            return hinted_raw(sizeBytes, opts.sectorSizeHint);
        }
        return hinted_raw(sizeBytes, opts.sectorSizeHint);
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
    registry.registerProbe(std::make_unique<DiskCopy42Probe>());
    registry.registerProbe(std::make_unique<AdfsOldMapProbe>());
    registry.registerProbe(std::make_unique<FatBpbSectorSizeProbe>());
    registry.registerProbe(std::make_unique<XexBootProbe>());
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
