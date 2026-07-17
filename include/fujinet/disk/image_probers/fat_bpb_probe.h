#pragma once

#include "fujinet/disk/image_probers/image_probe.h"

namespace fujinet::disk {

class FatBpbSectorSizeProbe final : public IImageProbe {
public:
    ImageProbeResult probe(
        fs::IFile& file,
        std::uint64_t sizeBytes,
        std::string_view path,
        const MountOptions& opts
    ) const override;
};

} // namespace fujinet::disk
