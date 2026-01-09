#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/core/core.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/devices/network_device_diagnostics.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <cctype>
#include <cstdio>
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

static fujinet::io::NetworkDevice* get_net_device(fujinet::core::FujinetCore& core)
{
    using fujinet::io::protocol::WireDeviceId;
    using fujinet::io::protocol::to_device_id;

    auto* dev = core.deviceManager().getDevice(to_device_id(WireDeviceId::NetworkService));
    return dynamic_cast<fujinet::io::NetworkDevice*>(dev);
}

class NetworkDiagnosticProvider final : public IDiagnosticProvider {
public:
    explicit NetworkDiagnosticProvider(fujinet::core::FujinetCore& core)
        : _core(core)
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

        return DiagResult::not_found("unknown net command");
    }

private:
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
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_network_diagnostic_provider(::fujinet::core::FujinetCore& core)
{
    return std::make_unique<NetworkDiagnosticProvider>(core);
}

} // namespace fujinet::diag


