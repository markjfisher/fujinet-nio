#include "fujinet/disk/image_probers/raw_image_probe.h"

#include "fujinet/disk/image_probers/fat_bpb_probe.h"

#include <utility>

namespace fujinet::disk {

bool RawImageProbeRegistry::registerProbe(std::unique_ptr<IRawImageProbe> probe)
{
    if (!probe) return false;
    _probes.push_back(std::move(probe));
    return true;
}

RawImageProbeResult RawImageProbeRegistry::probe(fs::IFile& file, std::uint64_t sizeBytes) const
{
    for (const auto& probe : _probes) {
        const auto result = probe->probe(file, sizeBytes);
        if (result.matched && result.geometry.sectorSize != 0 && result.geometry.sectorCount != 0) {
            return result;
        }
    }
    return {};
}

RawImageProbeRegistry make_default_raw_image_probe_registry()
{
    RawImageProbeRegistry registry;
    registry.registerProbe(std::make_unique<FatBpbSectorSizeProbe>());
    return registry;
}

RawImageProbeResult probe_raw_image_geometry(fs::IFile& file, std::uint64_t sizeBytes)
{
    static const RawImageProbeRegistry registry = make_default_raw_image_probe_registry();
    return registry.probe(file, sizeBytes);
}

} // namespace fujinet::disk
