#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "fujinet/disk/disk_types.h"
#include "fujinet/fs/filesystem.h"

namespace fujinet::disk {

enum class ImageProbeConfidence : std::uint8_t {
    None = 0,
    Extension,
    Hint,
    Content,
};

struct ImageProbeResult {
    bool matched{false};
    ImageType type{ImageType::Auto};
    DiskGeometry geometry{};
    ImageProbeConfidence confidence{ImageProbeConfidence::None};
};

class IImageProbe {
public:
    virtual ~IImageProbe() = default;
    virtual ImageProbeResult probe(
        fs::IFile& file,
        std::uint64_t sizeBytes,
        std::string_view path,
        const MountOptions& opts
    ) const = 0;
};

class ProbeRegistry {
public:
    bool registerProbe(std::unique_ptr<IImageProbe> probe);
    ImageProbeResult probe(
        fs::IFile& file,
        std::uint64_t sizeBytes,
        std::string_view path,
        const MountOptions& opts
    ) const;

private:
    std::vector<std::unique_ptr<IImageProbe>> _probes;
};

ProbeRegistry make_default_probe_registry();
ImageProbeResult probe_image(
    fs::IFile& file,
    std::uint64_t sizeBytes,
    std::string_view path,
    const MountOptions& opts
);

bool has_geometry(const DiskGeometry& geometry) noexcept;

} // namespace fujinet::disk
