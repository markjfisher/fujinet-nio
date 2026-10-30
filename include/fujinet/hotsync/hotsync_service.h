#pragma once

// The HotSync service: syncs a Palm that reaches it over the network (NetSync
// on TCP 14238) or through a cradle on the FujiBus serial line. A sync blocks
// on the Palm for minutes, so the service runs on a task of its own
// (platform::start_service_task), never in core.tick(). It runs one sync at a
// time.

#include "fujinet/hotsync/netsync_transport.h"
#include "fujinet/hotsync/session.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace fujinet::fs {
class IFileSystem;
}

namespace fujinet::net {
class ITcpSocketOps;
}

namespace fujinet::hotsync {

class CradleChannel;

// `hotsync.backup` in fujinet.yaml: false for an unknown value.
bool parse_backup_policy(const std::string& text, BackupPolicy& out);
const char* backup_policy_name(BackupPolicy policy);

struct HotSyncSettings {
    std::uint16_t port{kNetSyncPort}; // NetSync; 0 turns it off
    std::string root{"/hotsync"};    // folder on the filesystem given to the service
    std::string userName{"FujiNet"};
    BackupPolicy backup{BackupPolicy::Flagged};
    std::size_t maxDatabaseBytes{kDefaultMaxDatabaseBytes};
};

// Cradle timing. A window lends the line at 9600 baud and listens for a
// Palm's WAKEUP; between windows the line is the bus's.
struct CradleWindow {
    std::uint32_t listenMs{1500};
    std::uint32_t gapMs{1500};
};

// What the diagnostics console shows. A copy, safe to read from any thread.
struct HotSyncStatus {
    bool listening{false};
    bool syncing{false};
    std::uint16_t port{0};
    std::uint32_t syncs{0};    // syncs started
    std::uint32_t failures{0}; // syncs that did not reach EndOfSync
    std::uint32_t rejected{0}; // connections that never completed the NetSync handshake
    std::string lastSource;    // "network" or "cradle"
    std::string lastTime;      // local time the last sync ended, or empty
    std::string lastUser;
    std::string lastResult;    // e.g. "ok" or "link failure"
    int lastInstalled{0};
    int lastInstallFailures{0};
    int lastBackedUp{0};
    int lastBackupFailures{0};
    // Cradle: "off", "app" (a FujiBus host holds the line), "listening",
    // "syncing" or "idle".
    std::string cradle{"off"};
    std::uint32_t cradleWindows{0};
    // The service task's lowest free stack so far, in bytes; 0 when the
    // platform cannot tell.
    std::size_t taskStackMinFree{0};
};

class HotSyncService {
public:
    // Local wall-clock time for the Palm's "last synced" stamp; year 0 when
    // the clock is not set.
    using Clock = std::function<DlpDateTime()>;
    // The calling task's lowest free stack so far, for HotSyncStatus.
    using StackProbe = std::function<std::size_t()>;

    // `cradle` may be null: no cradle on this build or config.
    HotSyncService(net::ITcpSocketOps& ops, fs::IFileSystem& fs, HotSyncSettings settings, Clock clock,
                   CradleChannel* cradle = nullptr, CradleWindow window = {});
    ~HotSyncService();

    HotSyncService(const HotSyncService&) = delete;
    HotSyncService& operator=(const HotSyncService&) = delete;

    // One pass: serve one waiting network Palm, then open a cradle window if
    // one is due. Returns true when it did something, false when idle.
    bool poll();
    // Polls until stop() is called. For the service task.
    void run();
    void stop() { _stop.store(true); }

    HotSyncStatus status() const;
    // Called on the service task after each pass that did something.
    void set_stack_probe(StackProbe probe) { _stackProbe = std::move(probe); }

    // Downloads an http:// URL into the install queue for the next sync. The
    // file is named after the URL's last path segment unless `name` is given;
    // either must end in .prc, .pdb or .pqa. With `sha256` (64 hex digits) the
    // download is queued only if its digest matches, since plain HTTP lets the
    // network substitute it. Returns an empty string on success (`name` and
    // `size` describe the file), otherwise what went wrong. Blocks while it
    // downloads: for the console task.
    std::string fetch_install(const std::string& url, std::string& name, std::size_t& size,
                              const std::string& sha256 = {});
    const HotSyncSettings& settings() const { return _settings; }
    bool has_cradle() const { return _cradle != nullptr; }
    // Null without a cradle.
    const CradleChannel* cradle() const { return _cradle; }

private:
    bool poll_network();
    bool poll_cradle();
    bool start_listening();
    void serve(int fd);
    void run_session(DlpTransport& transport, const char* source);
    void set_cradle_state(const char* state);
    std::string now_text() const;

    net::ITcpSocketOps& _ops;
    fs::IFileSystem& _fs;
    HotSyncSettings _settings;
    Clock _clock;
    StackProbe _stackProbe;
    CradleChannel* _cradle;
    CradleWindow _window;
    int _listenFd{-1};
    std::uint64_t _nextListenAttemptMs{0};
    std::uint64_t _nextWindowMs{0};
    std::atomic_bool _stop{false};

    mutable std::mutex _statusMutex;
    HotSyncStatus _status;
};

} // namespace fujinet::hotsync
