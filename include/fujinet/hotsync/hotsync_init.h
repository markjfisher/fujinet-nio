#pragma once

// HotSync's one entry point for a platform main (docs/hotsync.md).
//
// Builds without FN_WITH_HOTSYNC link a stub (src/lib/hotsync_init_off.cpp)
// that does nothing, so a main calls start_hotsync() the same way in every
// build, and no HotSync code is built.

#include <memory>

namespace fujinet::config {
struct HotSyncConfig;
}

namespace fujinet::diag {
class DiagnosticRegistry;
}

namespace fujinet::fs {
class StorageManager;
}

namespace fujinet::io {
class Channel;
class FujiDevice;
}

namespace fujinet::hotsync {

// What HotSync needs from the main.
struct HotSyncHost {
    fs::StorageManager& storage;
    const config::HotSyncConfig& config;
    io::FujiDevice* fuji;                // for hotsync.get/set/save; may be null
    diag::DiagnosticRegistry& diagnostics;
};

// Everything HotSync started. The main keeps it for the program's life.
class HotSyncRuntime {
public:
    virtual ~HotSyncRuntime() = default;
};

// Call once the FujiBus line exists and before core::setup_transports, and
// give the transports `line` afterwards: with hotsync.cradle on, start_hotsync
// replaces it with the cradle that owns it. Starts the service on a task of its
// own when hotsync.enabled, and registers the hotsync.* console commands. Null
// `line` (a build with no FujiBus channel) is fine.
//
// Without FN_WITH_HOTSYNC it leaves `line` alone and returns null.
std::unique_ptr<HotSyncRuntime> start_hotsync(const HotSyncHost& host, std::unique_ptr<io::Channel>& line);

} // namespace fujinet::hotsync
