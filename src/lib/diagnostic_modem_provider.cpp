#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/core/core.h"
#include "fujinet/io/devices/modem_device.h"
#include "fujinet/io/devices/modem_device_diagnostics.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::diag {

namespace {

static fujinet::io::ModemDevice* get_modem_device(fujinet::core::FujinetCore& core)
{
    using fujinet::io::protocol::WireDeviceId;
    using fujinet::io::protocol::to_device_id;

    auto* dev = core.deviceManager().getDevice(to_device_id(WireDeviceId::ModemService));
    return dynamic_cast<fujinet::io::ModemDevice*>(dev);
}

static bool parse_u32(std::string_view s, std::uint32_t& out)
{
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
        if (v > 0xFFFFFFFFull) return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

class ModemDiagnosticProvider final : public IDiagnosticProvider {
public:
    explicit ModemDiagnosticProvider(fujinet::core::FujinetCore& core)
        : _core(core)
    {}

    std::string_view provider_id() const noexcept override { return "modem"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "modem.status",
            .summary = "show modem state (mode, listen, baud, cursors)",
            .usage = "modem.status",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "modem.at",
            .summary = "send an AT command and return the modem's response",
            .usage = "modem.at <command...>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "modem.drain",
            .summary = "drain pending modem output bytes (if any)",
            .usage = "modem.drain",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "modem.baud",
            .summary = "set modem baud (informational; affects CONNECT messaging)",
            .usage = "modem.baud <300|600|1200|1800|2400|4800|9600|19200>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "modem.baudlock",
            .summary = "enable/disable baud lock",
            .usage = "modem.baudlock <0|1>",
            .safe = false,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }

        const std::string_view cmd = args.argv[0];
        if (cmd == "modem.status") return cmd_status();
        if (cmd == "modem.at") return cmd_at(args);
        if (cmd == "modem.drain") return cmd_drain();
        if (cmd == "modem.baud") return cmd_baud(args);
        if (cmd == "modem.baudlock") return cmd_baudlock(args);

        return DiagResult::not_found("unknown modem command");
    }

private:
    DiagResult cmd_status()
    {
        auto* mdm = get_modem_device(_core);
        if (!mdm) return DiagResult::not_ready("ModemDevice not registered");

        const auto s = fujinet::io::ModemDeviceDiagnosticsAccessor::state(*mdm);

        std::string text;
        text.reserve(256);
        text += "cmd_mode: "; text += (s.cmdMode ? "1" : "0"); text += "\r\n";
        text += "connected: "; text += (s.connected ? "1" : "0"); text += "\r\n";
        text += "listening: "; text += (s.listening ? "1" : "0"); text += "\r\n";
        text += "pending: "; text += (s.pending ? "1" : "0"); text += "\r\n";
        text += "listen_port: "; text += std::to_string(s.listenPort); text += "\r\n";
        text += "telnet: "; text += (s.telnet ? "1" : "0"); text += "\r\n";
        text += "echo: "; text += (s.echo ? "1" : "0"); text += "\r\n";
        text += "numeric: "; text += (s.numeric ? "1" : "0"); text += "\r\n";
        text += "auto_answer: "; text += (s.autoAnswer ? "1" : "0"); text += "\r\n";
        text += "baud: "; text += std::to_string(s.baud); text += "\r\n";
        text += "baud_lock: "; text += (s.baudLock ? "1" : "0"); text += "\r\n";
        text += "host_write_cursor: "; text += std::to_string(s.hostWriteCursor); text += "\r\n";
        text += "host_read_cursor: "; text += std::to_string(s.hostReadCursor); text += "\r\n";
        text += "host_rx_avail: "; text += std::to_string(s.hostRxAvail); text += "\r\n";

        DiagResult r = DiagResult::ok(text);
        r.kv.emplace_back("cmd_mode", s.cmdMode ? "1" : "0");
        r.kv.emplace_back("connected", s.connected ? "1" : "0");
        r.kv.emplace_back("listen_port", std::to_string(s.listenPort));
        r.kv.emplace_back("baud", std::to_string(s.baud));
        r.kv.emplace_back("host_rx_avail", std::to_string(s.hostRxAvail));
        return r;
    }

    DiagResult cmd_drain()
    {
        auto* mdm = get_modem_device(_core);
        if (!mdm) return DiagResult::not_ready("ModemDevice not registered");

        std::string out = fujinet::io::ModemDeviceDiagnosticsAccessor::drain_output(*mdm);
        if (out.empty()) out = "(no output)\r\n";
        return DiagResult::ok(out);
    }

    DiagResult cmd_at(const DiagArgsView& args)
    {
        auto* mdm = get_modem_device(_core);
        if (!mdm) return DiagResult::not_ready("ModemDevice not registered");

        if (args.argv.size() < 2) {
            return DiagResult::invalid_args("usage: modem.at <command...>");
        }

        // Join args[1..] with spaces (user types: modem.at ATDT host:23)
        std::string line;
        line.reserve(64);
        for (std::size_t i = 1; i < args.argv.size(); ++i) {
            if (i > 1) line.push_back(' ');
            line.append(args.argv[i].data(), args.argv[i].size());
        }

        // Make it an AT command if user omitted "AT"
        if (line.size() < 2 || !(line[0] == 'A' || line[0] == 'a') || !(line[1] == 'T' || line[1] == 't')) {
            line.insert(line.begin(), 'T');
            line.insert(line.begin(), 'A');
        }
        if (line.empty() || (line.back() != '\r' && line.back() != '\n')) {
            line.push_back('\r');
        }

        fujinet::io::ModemDeviceDiagnosticsAccessor::inject_bytes(*mdm, line);

        // Run a short poll loop to let CONNECT/RING/etc materialize.
        for (int i = 0; i < 200; ++i) {
            mdm->poll();
        }

        std::string out = fujinet::io::ModemDeviceDiagnosticsAccessor::drain_output(*mdm);
        if (out.empty()) out = "(no output)\r\n";
        return DiagResult::ok(out);
    }

    DiagResult cmd_baud(const DiagArgsView& args)
    {
        auto* mdm = get_modem_device(_core);
        if (!mdm) return DiagResult::not_ready("ModemDevice not registered");
        if (args.argv.size() != 2) return DiagResult::invalid_args("usage: modem.baud <rate>");

        std::uint32_t b = 0;
        if (!parse_u32(args.argv[1], b)) return DiagResult::invalid_args("baud must be decimal");

        fujinet::io::ModemDeviceDiagnosticsAccessor::set_baud(*mdm, b);
        const auto s = fujinet::io::ModemDeviceDiagnosticsAccessor::state(*mdm);

        return DiagResult::ok("baud: " + std::to_string(s.baud) + "\r\n");
    }

    DiagResult cmd_baudlock(const DiagArgsView& args)
    {
        auto* mdm = get_modem_device(_core);
        if (!mdm) return DiagResult::not_ready("ModemDevice not registered");
        if (args.argv.size() != 2) return DiagResult::invalid_args("usage: modem.baudlock <0|1>");

        const bool en = (args.argv[1] == "1");
        if (!(args.argv[1] == "0" || args.argv[1] == "1")) {
            return DiagResult::invalid_args("expected 0 or 1");
        }

        fujinet::io::ModemDeviceDiagnosticsAccessor::set_baud_lock(*mdm, en);
        const auto s = fujinet::io::ModemDeviceDiagnosticsAccessor::state(*mdm);
        return DiagResult::ok(std::string("baud_lock: ") + (s.baudLock ? "1" : "0") + "\r\n");
    }

    fujinet::core::FujinetCore& _core;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_modem_diagnostic_provider(::fujinet::core::FujinetCore& core)
{
    return std::make_unique<ModemDiagnosticProvider>(core);
}

} // namespace fujinet::diag


