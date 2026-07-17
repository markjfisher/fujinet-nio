#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "fujinet/disk/disk_types.h"
#include "fujinet/fs/filesystem.h"

namespace fujinet::disk {

struct RawImageProbeResult {
    bool matched{false};
    DiskGeometry geometry{};
};

class IRawImageProbe {
public:
    virtual ~IRawImageProbe() = default;
    virtual RawImageProbeResult probe(fs::IFile& file, std::uint64_t sizeBytes) const = 0;
};

class RawImageProbeRegistry {
public:
    bool registerProbe(std::unique_ptr<IRawImageProbe> probe);
    RawImageProbeResult probe(fs::IFile& file, std::uint64_t sizeBytes) const;

private:
    std::vector<std::unique_ptr<IRawImageProbe>> _probes;
};

RawImageProbeRegistry make_default_raw_image_probe_registry();
RawImageProbeResult probe_raw_image_geometry(fs::IFile& file, std::uint64_t sizeBytes);

} // namespace fujinet::disk
