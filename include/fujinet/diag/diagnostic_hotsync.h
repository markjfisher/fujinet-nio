#pragma once

// HotSync diagnostics, kept out of diagnostic_provider.h because only
// FN_WITH_HOTSYNC builds have them.

#include "fujinet/diag/diagnostic_provider.h"

#include <memory>

namespace fujinet::hotsync {
class HotSyncService;
}

namespace fujinet::diag {

// Palm HotSync (docs/hotsync.md): service state and the last sync, and the
// stored settings through `fuji` (may be null). service is null when HotSync
// is off.
std::unique_ptr<IDiagnosticProvider> create_hotsync_diagnostic_provider(
    std::shared_ptr<::fujinet::hotsync::HotSyncService> service,
    ::fujinet::io::FujiDevice* fuji);

} // namespace fujinet::diag
