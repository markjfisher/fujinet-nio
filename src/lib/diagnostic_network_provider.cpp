#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/core/core.h"
#include "fujinet/diag/diagnostic_parse.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/devices/network_device_diagnostics.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/net/network_link.h"

#if !defined(FN_PLATFORM_POSIX)
#include "fujinet/platform/esp32/wifi_link.h"
#endif

#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::diag {

namespace {

static bool parse_u16(std::string_view s, std::uint16_t& out)
{
    // Accept decimal or 0x-prefixed hex.
    if (s.empty()) return false;

    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
        if (s.empty()) return false;
    }

    unsigned value = 0;
    for (char c : s) {
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
        else return false;

        value = value * static_cast<unsigned>(base) + static_cast<unsigned>(digit);
        if (value > 0xFFFFu) return false;
    }

    out = static_cast<std::uint16_t>(value);
    return true;
}

static std::string hex4(std::uint16_t v)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(v));
    return std::string(buf);
}

static const char* link_state_name(fujinet::net::LinkState st)
{
    using fujinet::net::LinkState;
    switch (st) {
    case LinkState::Disconnected: return "disconnected";
    case LinkState::Connecting:   return "connecting";
    case LinkState::Connected:    return "connected";
    case LinkState::Failed:     return "failed";
    }
    return "unknown";
}

static fujinet::io::NetworkDevice* get_net_device(fujinet::core::FujinetCore& core)
{
    using fujinet::io::protocol::WireDeviceId;
    using fujinet::io::protocol::to_device_id;

    auto* dev = core.deviceManager().getDevice(to_device_id(WireDeviceId::NetworkService));
    return dynamic_cast<fujinet::io::NetworkDevice*>(dev);
}

class NetworkDiagnosticProvider final : public IDiagnosticProvider {
public:
    NetworkDiagnosticProvider(fujinet::core::FujinetCore& core, std::shared_ptr<NetworkDiagWifiContext> wifi_ctx)
        : _core(core)
        , _wifi_ctx(std::move(wifi_ctx))
    {}

    std::string_view provider_id() const noexcept override { return "net"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "net.sessions",
            .summary = "list active network sessions/handles",
            .usage = "net.sessions",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "net.close",
            .summary = "close a session handle (or all)",
            .usage = "net.close <handle|all>",
            .safe = false,
        });
        if (_wifi_ctx) {
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.scan",
                .summary = "scan for nearby Wi-Fi access points",
                .usage = "net.wifi.scan",
                .safe = true,
            });
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.status",
                .summary = "show configured and live Wi-Fi state",
                .usage = "net.wifi.status",
                .safe = true,
            });
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.get",
                .summary = "show configured Wi-Fi SSID and password",
                .usage = "net.wifi.get",
                .safe = true,
            });
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.set",
                .summary = "set configured SSID or password, save, and reconnect",
                .usage = "net.wifi.set <ssid|password|passphrase> <value...>",
                .safe = false,
            });
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.save",
                .summary = "persist current Wi-Fi settings to fujinet.yaml",
                .usage = "net.wifi.save",
                .safe = false,
            });
        }
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }

        const std::string_view cmd = args.argv[0];
        if (cmd == "net.sessions") {
            return cmd_sessions();
        }
        if (cmd == "net.close") {
            return cmd_close(args);
        }
        if (_wifi_ctx) {
            if (cmd == "net.wifi.scan") {
                return cmd_wifi_scan();
            }
            if (cmd == "net.wifi.status") {
                return cmd_wifi_status();
            }
            if (cmd == "net.wifi.get") {
                return cmd_wifi_get();
            }
            if (cmd == "net.wifi.set") {
                return cmd_wifi_set(args);
            }
            if (cmd == "net.wifi.save") {
                return cmd_wifi_save();
            }
        }

        return DiagResult::not_found("unknown net command");
    }

private:
    fujinet::io::FujiDevice* fuji_device() const
    {
        return _wifi_ctx ? _wifi_ctx->fuji : nullptr;
    }

    fujinet::net::INetworkLink* wifi_link() const
    {
        if (!_wifi_ctx || !_wifi_ctx->ensure_wifi) {
            return nullptr;
        }
        return _wifi_ctx->ensure_wifi();
    }

    DiagResult reconnect_wifi()
    {
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }

        const auto& wifi_cfg = fuji->config().wifi;
        if (wifi_cfg.ssid.empty()) {
            return DiagResult::invalid_args("ssid not configured");
        }

        auto* link = wifi_link();
        if (!link) {
            return DiagResult::not_ready("Wi-Fi link not available");
        }

        fuji->config_mut().wifi.enabled = true;
        link->connect(wifi_cfg.ssid, wifi_cfg.passphrase);

        std::string text = "reconnecting to ssid='";
        text += wifi_cfg.ssid;
        text += "'\r\n";

        DiagResult r = DiagResult::ok(std::move(text));
        r.kv.emplace_back("ssid", wifi_cfg.ssid);
        return r;
    }

    static std::string join_args(const DiagArgsView& args, std::size_t from)
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

    void append_wifi_save_status(fujinet::io::FujiDevice* fuji, DiagResult& r)
    {
        auto* store = fuji->config_store();
        if (!store) {
            r.text += "warning: config store unavailable; not persisted\r\n";
            return;
        }

        store->save(fuji->config());
        r.text += "saved to config store\r\n";
    }

    DiagResult cmd_wifi_get()
    {
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }

        const auto& wifi_cfg = fuji->config().wifi;
        std::string text;
        text += "enabled: ";
        text += (wifi_cfg.enabled ? "1" : "0");
        text += "\r\nssid: ";
        text += wifi_cfg.ssid;
        text += "\r\npassword: ";
        text += wifi_cfg.passphrase;
        text += "\r\n";

        DiagResult r = DiagResult::ok(std::move(text));
        r.kv.emplace_back("enabled", wifi_cfg.enabled ? "1" : "0");
        r.kv.emplace_back("ssid", wifi_cfg.ssid);
        r.kv.emplace_back("password", wifi_cfg.passphrase);
        return r;
    }

    DiagResult cmd_wifi_status()
    {
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }

        const auto& wifi_cfg = fuji->config().wifi;
        auto* link = wifi_link();

        std::string text;
        text += "configured_enabled: ";
        text += (wifi_cfg.enabled ? "1" : "0");
        text += "\r\nconfigured_ssid: ";
        text += wifi_cfg.ssid;
        text += "\r\n";

        if (!link) {
            text += "link_state: unavailable\r\n";
            return DiagResult::ok(std::move(text));
        }

        text += "link_state: ";
        text += link_state_name(link->state());
        text += "\r\nip: ";
        text += link->ip_address();
        text += "\r\n";

        DiagResult r = DiagResult::ok(text);
        r.kv.emplace_back("link_state", link_state_name(link->state()));
        r.kv.emplace_back("ip", link->ip_address());
        return r;
    }

    DiagResult cmd_wifi_set(const DiagArgsView& args)
    {
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }

        if (args.argv.size() < 3) {
            return DiagResult::invalid_args("usage: net.wifi.set <ssid|password|passphrase> <value...>");
        }

        const std::string_view field = args.argv[1];
        const std::string value = join_args(args, 2);
        if (value.empty()) {
            return DiagResult::invalid_args("value must not be empty");
        }

        auto& wifi_cfg = fuji->config_mut().wifi;
        bool changed = false;
        const bool is_password_field =
            ascii_iequals(field, "password") || ascii_iequals(field, "passphrase");

        if (ascii_iequals(field, "ssid")) {
            if (wifi_cfg.ssid != value) {
                wifi_cfg.ssid = value;
                changed = true;
            }
        } else if (is_password_field) {
            if (wifi_cfg.passphrase != value) {
                wifi_cfg.passphrase = value;
                changed = true;
            }
        } else {
            return DiagResult::invalid_args("field must be ssid, password, or passphrase");
        }

        if (!changed) {
            return DiagResult::ok("unchanged\r\n");
        }

        wifi_cfg.enabled = true;

        std::string text;
        if (is_password_field) {
            text += "password updated\r\n";
        } else {
            text += "ssid updated\r\n";
        }

        auto* link = wifi_link();
        if (!link) {
            DiagResult r = DiagResult::ok(std::move(text));
            append_wifi_save_status(fuji, r);
            r.text += "warning: Wi-Fi link not available; reconnect skipped\r\n";
            return r;
        }

        link->connect(wifi_cfg.ssid, wifi_cfg.passphrase);
        text += "reconnecting to ssid='";
        text += wifi_cfg.ssid;
        text += "'\r\n";

        DiagResult r = DiagResult::ok(std::move(text));
        r.kv.emplace_back("ssid", wifi_cfg.ssid);
        append_wifi_save_status(fuji, r);
        return r;
    }

    DiagResult cmd_wifi_save()
    {
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }

        auto* store = fuji->config_store();
        if (!store) {
            return DiagResult::not_ready("config store not available");
        }

        store->save(fuji->config());
        return DiagResult::ok("saved wifi settings to config store\r\n");
    }

    DiagResult cmd_wifi_scan()
    {
#if defined(FN_PLATFORM_POSIX)
        (void)0;
        return DiagResult::not_ready("Wi-Fi scan not available on this platform");
#else
        auto* link = wifi_link();
        if (!link) {
            return DiagResult::not_ready("Wi-Fi link not available");
        }

        auto* wifi = dynamic_cast<fujinet::platform::esp32::Esp32WifiLink*>(link);
        if (!wifi) {
            return DiagResult::not_ready("Wi-Fi scan requires Esp32WifiLink");
        }

        const auto scan = wifi->scan();
        if (!scan.success) {
            return DiagResult::error(scan.error + "\r\n");
        }

        std::string text;
        text.reserve(scan.aps.size() * 72 + 32);
        text += "ap_count: ";
        text += std::to_string(scan.aps.size());
        text += "\r\n";

        for (const auto& ap : scan.aps) {
            text += "ssid=";
            text += ap.ssid;
            text += " rssi=";
            text += std::to_string(ap.rssi);
            text += " channel=";
            text += std::to_string(ap.channel);
            text += " auth=";
            text += ap.auth;
            text += "\r\n";
        }

        if (scan.aps.empty()) {
            text += "note: scan completed but no access points were reported\r\n";
        }

        DiagResult r = DiagResult::ok(std::move(text));
        r.kv.emplace_back("ap_count", std::to_string(scan.aps.size()));
        return r;
#endif
    }

    DiagResult cmd_sessions()
    {
        auto* net = get_net_device(_core);
        if (!net) {
            return DiagResult::not_ready("NetworkDevice not registered");
        }

        const auto rows = fujinet::io::NetworkDeviceDiagnosticsAccessor::sessions(*net);

        std::string text;
        text.reserve(256);

        std::size_t active = 0;
        for (const auto& r : rows) {
            if (!r.active) continue;
            ++active;
        }

        text += "active_sessions: ";
        text += std::to_string(active);
        text += "\r\n";

        for (const auto& r : rows) {
            if (!r.active) continue;

            text += "handle=";
            text += hex4(r.handle);
            text += " method=";
            text += std::to_string(r.method);
            text += " flags=";
            text += std::to_string(r.flags);
            text += " awaiting_body=";
            text += (r.awaitingBody ? "1" : "0");
            text += " body=";
            text += std::to_string(r.receivedBodyLen);
            text += "/";
            text += std::to_string(r.expectedBodyLen);
            text += " completed=";
            text += (r.completed ? "1" : "0");
            text += " url=";
            text += r.url;
            text += "\r\n";
        }

        DiagResult res = DiagResult::ok(text);
        res.kv.emplace_back("active_sessions", std::to_string(active));
        return res;
    }

    DiagResult cmd_close(const DiagArgsView& args)
    {
        auto* net = get_net_device(_core);
        if (!net) {
            return DiagResult::not_ready("NetworkDevice not registered");
        }

        // args.argv[0] = "net.close", args.argv[1] = handle|all
        if (args.argv.size() < 2) {
            return DiagResult::invalid_args("usage: net.close <handle|all>");
        }

        const std::string_view target = args.argv[1];
        if (target == "all") {
            const std::size_t n = fujinet::io::NetworkDeviceDiagnosticsAccessor::close_all(*net);
            DiagResult r = DiagResult::ok("closed: " + std::to_string(n) + "\r\n");
            r.kv.emplace_back("closed", std::to_string(n));
            return r;
        }

        std::uint16_t handle = 0;
        if (!parse_u16(target, handle)) {
            return DiagResult::invalid_args("invalid handle (expected decimal or 0xHHHH)");
        }

        const bool ok = fujinet::io::NetworkDeviceDiagnosticsAccessor::close(*net, handle);
        if (!ok) {
            return DiagResult::error("close failed (invalid handle?)\r\n");
        }

        DiagResult r = DiagResult::ok("closed: " + hex4(handle) + "\r\n");
        r.kv.emplace_back("closed_handle", hex4(handle));
        return r;
    }

    fujinet::core::FujinetCore& _core;
    std::shared_ptr<NetworkDiagWifiContext> _wifi_ctx;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_network_diagnostic_provider(
    ::fujinet::core::FujinetCore& core,
    std::shared_ptr<NetworkDiagWifiContext> wifi_ctx)
{
    return std::make_unique<NetworkDiagnosticProvider>(core, std::move(wifi_ctx));
}

} // namespace fujinet::diag
