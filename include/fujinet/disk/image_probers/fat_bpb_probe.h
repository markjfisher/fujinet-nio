#pragma once

#include "fujinet/disk/image_probers/raw_image_probe.h"

namespace fujinet::disk {

class FatBpbSectorSizeProbe final : public IRawImageProbe {
public:
    RawImageProbeResult probe(fs::IFile& file, std::uint64_t sizeBytes) const override;
};

} // namespace fujinet::disk
