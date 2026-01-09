#pragma once

#include "fujinet/io/devices/disk_device.h"

#include <cstddef>
#include <vector>

namespace fujinet::io {

// Friend accessor for DiskDevice internals, used by out-of-band diagnostics providers.
// Keeps diagnostic structures and helpers out of the main device header.
struct DiskDeviceDiagnosticsAccessor {
    static std::vector<disk::DiskSlotInfo> slots(const DiskDevice& dev)
    {
        std::vector<disk::DiskSlotInfo> out;
        const std::size_t n = dev._svc.slot_count();
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(dev._svc.info(i));
        }
        return out;
    }
};

} // namespace fujinet::io


