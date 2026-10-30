// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/diag/diagnostic_hotsync.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/diag/diagnostic_parse.h"
#include "fujinet/hotsync/cradle_channel.h"
#include "fujinet/hotsync/hotsync_service.h"
#include "fujinet/hotsync/session.h"
#include "fujinet/io/devices/fuji_device.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fujinet::diag {

namespace {

// Palm OS keeps at most 40 characters of user name.
constexpr std::size_t kMaxUserName = 40;

void add_line(DiagResult& r, const char* key, const std::string& value)
{
    r.text += key;
    r.text += ": ";
    r.text += value;
    r.text += "\r\n";
    r.kv.emplace_back(key, value);
}

std::string join_args(const DiagArgsView& args, std::size_t from)
{
    std::string out;
    for (std::size_t i = from; i < args.argv.size(); ++i) {
        if (i > from) {
            out.push_back(' ');
        }
        out.append(args.argv[i].data(), args.argv[i].size());
    }
    return out;
}

class HotSyncDiagnosticProvider final : public IDiagnosticProvider {
public:
    HotSyncDiagnosticProvider(std::shared_ptr<hotsync::HotSyncService> service, io::FujiDevice* fuji)
        : _service(std::move(service))
        , _fuji(fuji)
    {}

    std::string_view provider_id() const noexcept override { return "hotsync"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "hotsync.status",
            .summary = "show the HotSync service and the last sync",
            .usage = "hotsync.status",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "hotsync.get",
            .summary = "show the stored HotSync settings (applied at start-up)",
            .usage = "hotsync.get",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "hotsync.set",
            .summary = "change a stored HotSync setting; hotsync.save, then restart to apply",
            .usage = "hotsync.set <enabled|port|cradle|root|user_name|backup> <value...>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "hotsync.fetch",
            .summary = "download an app or database over HTTP into the queue for the next HotSync",
            .usage = "hotsync.fetch <http://host[:port]/file.prc> [name] [sha256=<64 hex digits>]",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "hotsync.save",
            .summary = "write the HotSync settings into fujinet.yaml (requires FujiDevice)",
            .usage = "hotsync.save",
            .safe = false,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }
        const std::string_view cmd = args.argv[0];
        if (cmd == "hotsync.status") return cmd_status();
        if (cmd == "hotsync.get") return cmd_get();
        if (cmd == "hotsync.set") return cmd_set(args);
        if (cmd == "hotsync.save") return cmd_save();
        if (cmd == "hotsync.fetch") return cmd_fetch(args);
        return DiagResult::not_found("unknown hotsync command");
    }

private:
    DiagResult cmd_status() const
    {
        DiagResult r = DiagResult::ok();
        if (!_service) {
            add_line(r, "enabled", "0");
            r.text += "(hotsync.set enabled 1, hotsync.save, then restart)\r\n";
            return r;
        }
        const hotsync::HotSyncSettings& settings = _service->settings();
        const hotsync::HotSyncStatus s = _service->status();
        add_line(r, "enabled", "1");
        add_line(r, "listening", s.listening ? "1" : "0");
        add_line(r, "port", std::to_string(s.port));
        add_line(r, "cradle", s.cradle);
        if (const hotsync::CradleChannel* cradle = _service->cradle()) {
            const hotsync::CradleStats c = cradle->stats();
            add_line(r, "cradle_windows", std::to_string(s.cradleWindows));
            add_line(r, "cradle_fujibus_packets", std::to_string(c.frames));
            add_line(r, "cradle_stray_bytes", std::to_string(c.strayBytes));
            add_line(r, "cradle_refused", std::to_string(c.refused));
            add_line(r, "cradle_dropped_writes", std::to_string(c.droppedWrites));
        }
        add_line(r, "root", settings.root);
        add_line(r, "user_name", settings.userName);
        add_line(r, "backup", hotsync::backup_policy_name(settings.backup));
        add_line(r, "syncing", s.syncing ? "1" : "0");
        add_line(r, "syncs", std::to_string(s.syncs));
        add_line(r, "failures", std::to_string(s.failures));
        add_line(r, "rejected", std::to_string(s.rejected));
        if (s.syncs > 0 && !s.syncing) {
            add_line(r, "last_source", s.lastSource);
            add_line(r, "last_time", s.lastTime.empty() ? "(clock not set)" : s.lastTime);
            add_line(r, "last_user", s.lastUser);
            add_line(r, "last_result", s.lastResult);
            add_line(r, "last_installed", std::to_string(s.lastInstalled));
            add_line(r, "last_install_failures", std::to_string(s.lastInstallFailures));
            add_line(r, "last_backed_up", std::to_string(s.lastBackedUp));
            add_line(r, "last_backup_failures", std::to_string(s.lastBackupFailures));
        }
        if (s.taskStackMinFree > 0) {
            add_line(r, "task_stack_min_free", std::to_string(s.taskStackMinFree));
        }
        return r;
    }

    DiagResult cmd_fetch(const DiagArgsView& args)
    {
        static constexpr const char* kUsage =
            "usage: hotsync.fetch <http://host[:port]/file.prc> [name] [sha256=<64 hex digits>]";
        if (args.argv.size() < 2 || args.argv.size() > 4) {
            return DiagResult::invalid_args(kUsage);
        }
        std::string name;
        std::string sha256;
        for (std::size_t i = 2; i < args.argv.size(); ++i) {
            const std::string_view arg = args.argv[i];
            if (arg.rfind("sha256=", 0) == 0) {
                sha256 = std::string(arg.substr(7));
                if (sha256.size() != 64 || sha256.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                    return DiagResult::invalid_args("sha256= takes 64 hex digits");
                }
            } else if (name.empty()) {
                name = std::string(arg);
            } else {
                return DiagResult::invalid_args(kUsage);
            }
        }
        if (!_service) {
            return DiagResult::not_ready("HotSync is off (hotsync.set enabled 1, hotsync.save, then restart)");
        }
        std::size_t size = 0;
        const std::string err = _service->fetch_install(std::string(args.argv[1]), name, size, sha256);
        if (!err.empty()) {
            return DiagResult::error("fetch failed: " + err);
        }
        DiagResult r = DiagResult::ok();
        add_line(r, "queued", name);
        add_line(r, "bytes", std::to_string(size));
        if (!sha256.empty()) {
            add_line(r, "sha256", "ok");
        }
        r.text += "(installed at the next HotSync)\r\n";
        return r;
    }

    DiagResult cmd_get() const
    {
        if (!_fuji) return DiagResult::not_ready("FujiDevice not available");
        const config::HotSyncConfig& c = _fuji->config().hotsync;
        DiagResult r = DiagResult::ok();
        add_line(r, "enabled", c.enabled ? "1" : "0");
        add_line(r, "port", std::to_string(c.port));
        add_line(r, "cradle", c.cradle ? "1" : "0");
        add_line(r, "root", c.root);
        add_line(r, "user_name", c.userName);
        add_line(r, "backup", c.backup);
        return r;
    }

    DiagResult cmd_set(const DiagArgsView& args)
    {
        static constexpr const char* kUsage =
            "usage: hotsync.set <enabled|port|cradle|root|user_name|backup> <value...>";
        if (args.argv.size() < 3) {
            return DiagResult::invalid_args(kUsage);
        }
        if (!_fuji) return DiagResult::not_ready("FujiDevice not available");
        config::HotSyncConfig& c = _fuji->config_mut().hotsync;
        const std::string_view field = args.argv[1];
        const std::string value = join_args(args, 2);

        if (ascii_iequals(field, "enabled") || ascii_iequals(field, "cradle")) {
            if (value != "0" && value != "1") {
                return DiagResult::invalid_args(std::string(field) + " must be 0 or 1");
            }
            (ascii_iequals(field, "enabled") ? c.enabled : c.cradle) = value == "1";
        } else if (ascii_iequals(field, "port")) {
            std::uint32_t port = 0;
            if (!parse_decimal_u32(value, port) || port > 65535) {
                return DiagResult::invalid_args("port must be 0..65535 (0 turns NetSync off)");
            }
            c.port = static_cast<std::uint16_t>(port);
        } else if (ascii_iequals(field, "root")) {
            if (value.empty() || value.front() != '/') {
                return DiagResult::invalid_args("root must start with /");
            }
            c.root = value;
        } else if (ascii_iequals(field, "user_name")) {
            if (value.size() > kMaxUserName) {
                return DiagResult::invalid_args("user_name is at most 40 characters");
            }
            c.userName = value;
        } else if (ascii_iequals(field, "backup")) {
            std::string lower = value;
            for (char& ch : lower) {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            }
            hotsync::BackupPolicy policy{};
            if (!hotsync::parse_backup_policy(lower, policy)) {
                return DiagResult::invalid_args("backup must be none, flagged or all");
            }
            c.backup = hotsync::backup_policy_name(policy);
        } else {
            return DiagResult::invalid_args(kUsage);
        }
        return DiagResult::ok(std::string(field) + " stored; hotsync.save, then restart to apply\r\n");
    }

    DiagResult cmd_save()
    {
        if (!_fuji) return DiagResult::not_ready("FujiDevice not available");
        auto* store = _fuji->config_store();
        if (!store) return DiagResult::not_ready("config store not available");
        store->save(_fuji->config());
        return DiagResult::ok("saved hotsync settings to config store; restart to apply\r\n");
    }

    std::shared_ptr<hotsync::HotSyncService> _service;
    io::FujiDevice* _fuji;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_hotsync_diagnostic_provider(
    std::shared_ptr<hotsync::HotSyncService> service,
    io::FujiDevice* fuji)
{
    return std::make_unique<HotSyncDiagnosticProvider>(std::move(service), fuji);
}

} // namespace fujinet::diag

#endif // FN_WITH_HOTSYNC
