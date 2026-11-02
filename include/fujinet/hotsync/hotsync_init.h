#pragma once

// HotSync's one entry point for a platform main (docs/hotsync.md).
//
// The build selects its definition: src/lib/hotsync/hotsync_init.cpp with
// FN_WITH_HOTSYNC, otherwise the stub in src/lib/hotsync_init_off.cpp, which
// does nothing. A main calls start_hotsync() the same way in every build.

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
class IBusLineCoordinator;
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

    // With the cradle on, what core::setup_transports gives the FujiBus
    // transport, so the cradle can share its line; otherwise null.
    virtual io::IBusLineCoordinator* bus_line_coordinator() { return nullptr; }
};

// Call once the FujiBus line exists and before core::setup_transports. Then
// give the transports `line` and the runtime's bus_line_coordinator(): with
// hotsync.cradle on, start_hotsync replaces `line` with the LineArbiter that
// owns it, and the coordinator decides when the cradle may borrow it. Starts
// the service on a task of its own when hotsync.enabled, and registers the
// hotsync.* console commands. Null `line` (a build with no FujiBus channel)
// is fine.
//
// The stub (no FN_WITH_HOTSYNC) leaves `line` alone and returns null.
std::unique_ptr<HotSyncRuntime> start_hotsync(const HotSyncHost& host, std::unique_ptr<io::Channel>& line);

} // namespace fujinet::hotsync
