#include "fujinet/hotsync/hotsync_init.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/core/logging.h"
#include "fujinet/diag/diagnostic_hotsync.h"
#include "fujinet/diag/diagnostic_registry.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/hotsync/cradle_coordinator.h"
#include "fujinet/hotsync/hotsync_service.h"
#include "fujinet/hotsync/line_arbiter.h"
#include "fujinet/io/core/baud_control.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/platform/memory_stats.h"
#include "fujinet/platform/service_task.h"
#include "fujinet/platform/tcp_socket_ops.h"
#include "fujinet/platform/time.h"

#include <chrono>
#include <ctime>
#include <memory>
#include <string>

namespace fujinet::hotsync {

namespace {

constexpr const char* TAG = "hotsync";
constexpr std::size_t kTaskStackBytes = 8192;
// How long shutdown waits for the service task. A cradle sync ends at once
// (its line is closed); a NetSync session ends at its next timeout.
constexpr auto kStopWait = std::chrono::seconds(10);

// Local time from the system clock and zone (clock.timezone).
DlpDateTime local_now()
{
    DlpDateTime t;
    if (!platform::time_is_valid()) {
        return t;
    }
    const auto now = static_cast<std::time_t>(platform::unix_time_seconds());
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr) {
        return t;
    }
    t.year = static_cast<std::uint16_t>(local.tm_year + 1900);
    t.month = static_cast<std::uint8_t>(local.tm_mon + 1);
    t.day = static_cast<std::uint8_t>(local.tm_mday);
    t.hour = static_cast<std::uint8_t>(local.tm_hour);
    t.minute = static_cast<std::uint8_t>(local.tm_min);
    t.second = static_cast<std::uint8_t>(local.tm_sec);
    return t;
}

// The service task's lowest free stack so far; 0 where the platform cannot
// tell (POSIX).
std::size_t task_stack_min_free()
{
    const platform::MemoryStats m = platform::memory_stats();
    return m.available ? m.taskStackMinFreeBytes : 0;
}

void run_service(void* ctx)
{
    auto* service = static_cast<std::shared_ptr<HotSyncService>*>(ctx);
    (*service)->run();
    delete service;
}

// With HotSync and its cradle on, puts the FujiBus line under a LineArbiter,
// which `line` then holds, and returns the coordinator that lends it; the line
// must change rate (io::BaudControl). Otherwise leaves `line` alone and
// returns null.
std::unique_ptr<CradleCoordinator> attach_cradle(std::unique_ptr<io::Channel>& line,
                                                 const config::HotSyncConfig& config)
{
    if (!config.enabled || !config.cradle || !line) {
        return nullptr;
    }
    auto* baud = dynamic_cast<io::BaudControl*>(line.get());
    if (baud == nullptr) {
        FN_LOGW(TAG, "hotsync.cradle needs a serial line that can change rate; no cradle");
        return nullptr;
    }
    auto arbiter = std::make_unique<LineArbiter>(std::move(line), *baud);
    auto coordinator = std::make_unique<CradleCoordinator>(*arbiter);
    line = std::move(arbiter);
    FN_LOGI(TAG, "Palm cradle shares the FujiBus line");
    return coordinator;
}

// Starts the service on a task of its own when config.enabled, with its folder
// on the persistent filesystem: NetSync unless config.port is 0, and the
// cradle when `cradle` is given. Null when HotSync is off or cannot start. The
// task keeps its own reference, so the service outlives the returned pointer.
std::shared_ptr<HotSyncService> start_service(fs::StorageManager& storage, const config::HotSyncConfig& config,
                                              CradleCoordinator* cradle)
{
    if (!config.enabled) {
        return nullptr;
    }
    fs::IFileSystem* fs = storage.defaultPersistentFileSystem();
    if (fs == nullptr) {
        FN_LOGW(TAG, "no persistent filesystem; HotSync is off");
        return nullptr;
    }
    // The service writes from its own task; only local filesystems may be
    // used from two tasks at once (fs::IFileSystem).
    if (fs::is_network_kind(fs->kind())) {
        FN_LOGW(TAG, "persistent filesystem %s is a network one; HotSync is off", fs->name().c_str());
        return nullptr;
    }

    HotSyncSettings settings;
    settings.port = config.port;
    settings.root = config.root.empty() ? "/hotsync" : config.root;
    settings.userName = config.userName.empty() ? "FujiNet" : config.userName;
    if (!parse_backup_policy(config.backup, settings.backup)) {
        FN_LOGW(TAG, "unknown hotsync.backup '%s'; using flagged", config.backup.c_str());
    }

    auto service = std::make_shared<HotSyncService>(platform::default_tcp_socket_ops(), *fs, settings,
                                                    &local_now, cradle);
    service->set_stack_probe(&task_stack_min_free);
    auto* taskRef = new std::shared_ptr<HotSyncService>(service);
    if (!platform::start_service_task("fn_hotsync", kTaskStackBytes, &run_service, taskRef)) {
        FN_LOGE(TAG, "cannot start the HotSync task");
        delete taskRef;
        return nullptr;
    }
    FN_LOGI(TAG, "HotSync on %s:%s, backup %s, NetSync %s, cradle %s", fs->name().c_str(),
            settings.root.c_str(), backup_policy_name(settings.backup),
            settings.port != 0 ? std::to_string(settings.port).c_str() : "off", cradle ? "on" : "off");
    return service;
}

// The service task holds its own reference to the service, so the service
// can outlive this. Before the coordinator goes, the destructor stops the
// task and waits for it; if it will not stop, the coordinator is left alive
// for it (the program is ending anyway).
class Runtime final : public HotSyncRuntime {
public:
    std::unique_ptr<CradleCoordinator> cradle;
    std::shared_ptr<HotSyncService> service;
    std::unique_ptr<diag::IDiagnosticProvider> diagnostics;

    ~Runtime() override
    {
        if (!service) {
            return;
        }
        service->stop();
        if (cradle) {
            cradle->shutdown();
        }
        if (!service->wait_stopped(kStopWait)) {
            FN_LOGW(TAG, "the HotSync task did not stop; leaving it running");
            (void)cradle.release();
        }
    }

    io::IBusLineCoordinator* bus_line_coordinator() override { return cradle.get(); }
};

} // namespace

std::unique_ptr<HotSyncRuntime> start_hotsync(const HotSyncHost& host, std::unique_ptr<io::Channel>& line)
{
    auto runtime = std::make_unique<Runtime>();
    runtime->cradle = attach_cradle(line, host.config);
    runtime->service = start_service(host.storage, host.config, runtime->cradle.get());
    runtime->diagnostics = diag::create_hotsync_diagnostic_provider(runtime->service, host.fuji);
    host.diagnostics.add_provider(*runtime->diagnostics);
    return runtime;
}

} // namespace fujinet::hotsync
