// Only the physical Atari SIO build registers this provider; host builds keep it so the unit
// tests can run it. Every other build compiles this file to nothing.
#if defined(FN_BUILD_ATARI_FUJIBUS_SIO) || defined(FN_PLATFORM_POSIX)

#include "fujinet/diag/diagnostic_atari_sio.h"
#include "fujinet/io/atari_sio_speed.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fujinet::diag {

namespace {

std::string hex2(std::uint8_t v)
{
    static const char digits[] = "0123456789abcdef";
    std::string s = "0x";
    s += digits[v >> 4];
    s += digits[v & 0x0f];
    return s;
}

// Names the SIO commands an Atari disk or the native endpoint is likely to send.
const char* command_name(std::uint8_t device, std::uint8_t command)
{
    if (command == 0x3f) return "high-speed query";
    if (device == 0x7f) {
        switch (command) {
        case 'Q': return "query";
        case 'W': return "write";
        case 'P': return "poll";
        case 'R': return "read";
        default: return nullptr;
        }
    }
    switch (command) {
    case 'S': return "status";
    case 'R': return "read sector";
    case 'W': return "write sector";
    case 'P': return "put sector";
    case 'N': return "read PERCOM";
    case 'O': return "write PERCOM";
    case '!': return "format";
    case '"': return "format medium";
    default: return nullptr;
    }
}

// 3100 us -> "3.1 ms"
std::string ms1(std::uint32_t us)
{
    return std::to_string(us / 1000) + "." + std::to_string((us % 1000) / 100) + " ms";
}

// An instant since boot: 2235000 us -> "2235"; 0 means it has not happened.
std::string at_ms(std::uint32_t us)
{
    return us ? std::to_string(us / 1000) : std::string("none");
}

void line(std::string& out, std::string_view text)
{
    out += text;
    out += "\r\n";
}

// POKEY clocks the serial port from the machine clock: bit rate = clock / (2 * (index + 7)) for
// high-speed index 0..10. The legacy firmware targets the PAL clock and adds 3 CPU cycles per byte
// to compensate for late sampling.
constexpr std::uint32_t pal_clock_hz = 1773447;
constexpr std::uint32_t ntsc_clock_hz = 1789790;
constexpr std::uint32_t max_baud_table_index = 10;  // the legacy table; the speed setting stops at 9

std::uint32_t pokey_baud(std::uint32_t clock_hz, std::uint32_t index)
{
    return clock_hz / (2 * (index + 7));
}

// Signed error of `actual` against `reference`, in tenths of a percent: "+0.1%".
std::string error_pct(std::uint32_t actual, std::uint32_t reference)
{
    const long long diff = static_cast<long long>(actual) - static_cast<long long>(reference);
    long long tenths = diff * 1000 / static_cast<long long>(reference);
    std::string s = tenths < 0 ? "-" : "+";
    if (tenths < 0) tenths = -tenths;
    return s + std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "%";
}

class AtariSioDiagnosticProvider final : public IDiagnosticProvider {
public:
    AtariSioDiagnosticProvider(std::function<io::AtariSioStats()> snapshot, std::function<void()> reset,
                               std::function<std::uint32_t(std::uint32_t)> probe_baud,
                               std::function<bool(int, bool)> set_hsio_index,
                               std::function<void(const io::AtariSioTuning&)> set_tuning,
                               std::function<std::string(int, const io::AtariSioTuning&)> save,
                               std::function<bool(bool)> set_proc, std::function<bool(unsigned, unsigned, unsigned)> pulse_proc)
        : _snapshot(std::move(snapshot)), _reset(std::move(reset)), _probe_baud(std::move(probe_baud)),
          _set_hsio_index(std::move(set_hsio_index)), _set_tuning(std::move(set_tuning)), _save(std::move(save)),
          _set_proc(std::move(set_proc)), _pulse_proc(std::move(pulse_proc))
    {}

    std::string_view provider_id() const noexcept override { return "sio"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "sio.status",
            .summary = "Atari SIO ingress counters: CMD line, UART, frames, aborts, last command",
            .usage = "sio.status",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "sio.trace", .summary = "recent SIO frames and capture aborts", .usage = "sio.trace", .safe = true,
        });
        if (_probe_baud) {
            out.push_back(DiagCommandSpec{
                .name = "sio.baud",
                .summary = "high-speed SIO baud check: UART rates against POKEY index rates (Atari idle)",
                .usage = "sio.baud",
                .safe = false,
            });
        }
        if (_set_hsio_index) {
            out.push_back(DiagCommandSpec{
                .name = "sio.hsio",
                .summary = "high-speed SIO: set the index answered to 3F (0-9) or off; not saved",
                .usage = "sio.hsio [<0-9> [nofallback]|off]",
                .safe = false,
            });
        }
        if (_set_tuning) {
            out.push_back(DiagCommandSpec{
                .name = "sio.tune",
                .summary = "SIO dials: ACK gap, data gap (us), second-listener window (ms), write-glitch filter, PROC notify; not saved",
                .usage = "sio.tune [ack <us>|data <us>|window <ms>|glitch <0|1>|proc <0|1>|reset]",
                .safe = false,
            });
        }
        if (_set_proc && _pulse_proc) {
            out.push_back(DiagCommandSpec{
                .name = "sio.proc",
                .summary = "PROC (proceed) line to the Atari: show it, assert (low), release, or pulse it (Atari idle)",
                .usage = "sio.proc [low|high|pulse [<count> [<low ms> [<high ms>]]]]",
                .safe = false,
            });
        }
        if (_save) {
            out.push_back(DiagCommandSpec{
                .name = "sio.save",
                .summary = "save the high-speed index and the sio.tune values to the config (fujinet.yaml, atari_sio)",
                .usage = "sio.save",
                .safe = false,
            });
        }
        out.push_back(DiagCommandSpec{
            .name = "sio.reset",
            .summary = "zero the SIO counters (do this before powering the Atari)",
            .usage = "sio.reset",
            .safe = false,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }
        const std::string_view cmd = args.argv[0];
        if (cmd == "sio.status") {
            return status();
        }
        if (cmd == "sio.trace") return trace();
        if (cmd == "sio.baud" && _probe_baud) {
            return baud();
        }
        if (cmd == "sio.hsio" && _set_hsio_index) {
            return hsio(args);
        }
        if (cmd == "sio.tune" && _set_tuning) {
            return tune(args);
        }
        if (cmd == "sio.proc" && _set_proc && _pulse_proc) {
            return proc(args);
        }
        if (cmd == "sio.save" && _save) {
            const io::AtariSioStats s = _snapshot();
            const std::string error = _save(s.hsio_index, s.tuning);
            if (!error.empty()) return DiagResult::not_ready(error);
            return DiagResult::ok("saved atari_sio (hsio_index " + std::to_string(s.hsio_index) + ", ack " +
                                  std::to_string(s.tuning.ack_gap_us) + " us, data " +
                                  std::to_string(s.tuning.data_gap_us) + " us, window " +
                                  std::to_string(s.tuning.alt_window_us / 1000) + " ms) to the config store\r\n");
        }
        if (cmd == "sio.reset") {
            _reset();
            return DiagResult::ok("sio counters reset\r\n");
        }
        return DiagResult::not_found("unknown sio command");
    }

private:
    DiagResult trace() const
    {
        const auto s = _snapshot();
        std::string text;
        const auto count = s.capture.trace_count;
        const auto size = static_cast<std::uint32_t>(s.capture.trace.size());
        const auto first = count > size ? count - size : 0;
        line(text, "events: frame=1 edge=2 timeout=3 overflow=4 uart=5 idle-edge=6; times are microseconds since boot (wraps)");
        for (auto i = first; i < count; ++i) {
            const auto& t = s.capture.trace[i % size];
            line(text, "us=" + std::to_string(t.time_us) + " event=" + std::to_string(t.event) +
                 " edges=" + std::to_string(t.edges) + " low=" + std::to_string(t.low) +
                 " collecting=" + std::to_string(t.collecting) + " released=" + std::to_string(t.released) +
                 " used=" + std::to_string(t.used) +
                 " pending=" + std::to_string(t.pending) + " frame=" + frame_text(t.frame));
        }
        return DiagResult::ok(std::move(text));
    }
    static std::string tuning_text(const io::AtariSioTuning& t)
    {
        return "tune: ack gap " + std::to_string(t.ack_gap_us) + " us, data gap " + std::to_string(t.data_gap_us) +
               " us (high speed only), second-listener window " + std::to_string(t.alt_window_us / 1000) + " ms, ignore CMD edges during a write " +
               (t.ignore_write_glitches ? "on" : "off") + ", PROC notify " + (t.proc_notify ? "on" : "off") +
               ((t.ack_gap_us != 250 || t.data_gap_us != 0 || t.alt_window_us != 10000 ||
                 !t.ignore_write_glitches || t.proc_notify) ? "   (not the defaults)" : "");
    }

    DiagResult tune(const DiagArgsView& args) const
    {
        const char* usage = "usage: sio.tune [ack <us>|data <us>|window <ms>|glitch <0|1>|proc <0|1>|reset]";
        io::AtariSioTuning t = _snapshot().tuning;
        if (args.argv.size() == 2 && args.argv[1] == "reset") {
            t = io::AtariSioTuning{};
        } else if (args.argv.size() == 3) {
            const std::string_view value = args.argv[2];
            if (value.empty() || value.size() > 6 || value.find_first_not_of("0123456789") != std::string_view::npos) {
                return DiagResult::invalid_args(usage);
            }
            const auto n = static_cast<std::uint32_t>(std::stoul(std::string(value)));
            if (args.argv[1] == "ack") t.ack_gap_us = n;
            else if (args.argv[1] == "data") t.data_gap_us = n;
            else if (args.argv[1] == "window") t.alt_window_us = n * 1000;
            else if (args.argv[1] == "glitch") t.ignore_write_glitches = n != 0;
            else if (args.argv[1] == "proc") t.proc_notify = n != 0;
            else return DiagResult::invalid_args(usage);
        } else if (args.argv.size() != 1) {
            return DiagResult::invalid_args(usage);
        }
        if (args.argv.size() > 1) _set_tuning(t);
        std::string text;
        line(text, tuning_text(t));
        return DiagResult::ok(std::move(text));
    }

    static std::string proc_line(const io::AtariSioStats& s)
    {
        if (!s.proc_available) return "proc: this board maps no PROC pin\r\n";
        return std::string("proc: ") + (s.proc_low ? "ASSERTED (low)" : "released (high)") + ", driven low " +
               std::to_string(s.proc_asserts) + " times since boot; notify " + (s.tuning.proc_notify ? "on" : "off") +
               ": " + std::to_string(s.proc_notices) + " readable notices, " + std::to_string(s.proc_pulses) +
               " pulses (sio.proc low|high|pulse, sio.tune proc 0|1)\r\n";
    }

    // sio.proc: the only way to cause a PROC edge until the protocol uses the line (docs/atari/proc.md).
    // A pulse blocks the SIO task for its whole length, so the bounds are small and the Atari should be
    // idle (waiting for a key).
    DiagResult proc(const DiagArgsView& args) const
    {
        const char* usage = "usage: sio.proc [low|high|pulse [<count 1-100> [<low ms 1-1000> [<high ms 1-1000>]]]]";
        if (args.argv.size() == 1) return DiagResult::ok(proc_line(_snapshot()));
        const std::string_view what = args.argv[1];
        if (what == "low" || what == "high") {
            if (args.argv.size() != 2) return DiagResult::invalid_args(usage);
            if (!_set_proc(what == "low")) return DiagResult::not_ready("this board maps no PROC pin");
            return DiagResult::ok(proc_line(_snapshot()));
        }
        if (what != "pulse" || args.argv.size() > 5) return DiagResult::invalid_args(usage);
        unsigned v[3] = {1, 10, 10};
        const unsigned max[3] = {100, 1000, 1000};
        for (std::size_t i = 2; i < args.argv.size(); ++i) {
            const std::string_view value = args.argv[i];
            if (value.empty() || value.size() > 4 || value.find_first_not_of("0123456789") != std::string_view::npos) {
                return DiagResult::invalid_args(usage);
            }
            v[i - 2] = static_cast<unsigned>(std::stoul(std::string(value)));
            if (v[i - 2] < 1 || v[i - 2] > max[i - 2]) return DiagResult::invalid_args(usage);
        }
        if (!_pulse_proc(v[0], v[1], v[2])) return DiagResult::not_ready("this board maps no PROC pin");
        std::string text = "pulsed PROC " + std::to_string(v[0]) + " time(s), low " + std::to_string(v[1]) + " ms, high " +
                           std::to_string(v[2]) + " ms\r\n";
        return DiagResult::ok(text + proc_line(_snapshot()));
    }

    DiagResult hsio(const DiagArgsView& args) const
    {
        if (args.argv.size() > 1) {
            const std::string_view value = args.argv[1];
            int index = -1;
            if (value != "off") {
                if (value.empty() || value.size() > 2 || value.find_first_not_of("0123456789") != std::string_view::npos) {
                    return DiagResult::invalid_args("usage: sio.hsio [<0-9> [nofallback]|off]");
                }
                index = std::stoi(std::string(value));
            }
            bool fallback = true;
            if (args.argv.size() > 2) {
                if (args.argv[2] != "nofallback" || index < 0) {
                    return DiagResult::invalid_args("usage: sio.hsio [<0-9> [nofallback]|off]");
                }
                fallback = false;
            }
            if (!_set_hsio_index(index, fallback)) {
                return DiagResult::invalid_args("index must be 0-9 (10 is read by drivers as a Happy 1050)");
            }
            std::string text = index < 0 ? "high-speed SIO off"
                                         : "high-speed SIO index " + std::to_string(index) + " (" +
                                               std::to_string(io::AtariSioSpeed::baud_for_index(
                                                   static_cast<unsigned>(index))) + " baud)";
            if (!fallback) text += " with no fallback to 19200";
            line(text, "; takes effect when the Atari next asks (3F), and is not saved");
            return DiagResult::ok(std::move(text));
        }
        return DiagResult::ok(hsio_line(_snapshot()));
    }

    static std::string frame_text(const std::array<std::uint8_t, 5>& f)
    {
        std::string text;
        for (std::size_t i = 0; i < f.size(); ++i) text += (i ? " " : "") + hex2(f[i]);
        text += " (device " + hex2(f[0]) + ", command " + hex2(f[1]);
        if (const char* name = command_name(f[0], f[1])) text += std::string(" ") + name;
        return text + ")";
    }

    static std::string bad_frame_text(const io::AtariSioBadFrame& b)
    {
        using Reason = io::AtariSioBadFrame::Reason;
        std::string text = b.reason == Reason::Checksum   ? "bad checksum"
                           : b.reason == Reason::Overflow ? "more than 5 bytes"
                           : b.reason == Reason::RxError  ? "UART error"
                           : b.reason == Reason::Short    ? "frame ended short"
                                                          : "none";
        text += ", " + std::to_string(b.length) + " byte(s):";
        for (std::size_t i = 0; i < b.bytes.size() && i < b.length; ++i) text += " " + hex2(b.bytes[i]);
        return text;
    }

    static std::string hsio_line(const io::AtariSioStats& s)
    {
        std::string text;
        if (s.hsio_index < 0) {
            line(text, "hsio: off (3F is NAKed; the Atari stays at 19200). sio.hsio <0-9> turns it on");
            return text;
        }
        line(text, "hsio: index " + std::to_string(s.hsio_index) + " (" +
                       std::to_string(io::AtariSioSpeed::baud_for_index(static_cast<unsigned>(s.hsio_index))) +
                       " baud), " + (s.hsio_high ? "HIGH SPEED now" : "standard speed now") +
                       "   switches " + std::to_string(s.hsio_switches) + ", fallbacks " +
                       std::to_string(s.hsio_fallbacks) + ", unreadable command frames " +
                       std::to_string(s.capture.frames_bad) + (s.hsio_fallback_enabled ? "" : "   (fallback off)"));
        line(text, "  3F asked: D1:-D8: " + std::to_string(s.router.hsio_queries_disk) + ", 0x7f " +
                       std::to_string(s.router.hsio_queries_nio));
        if (s.capture.frames_bad != 0) {
            line(text, "  first unreadable: " + bad_frame_text(s.capture.first_bad));
            line(text, "  last unreadable:  " + bad_frame_text(s.capture.last_bad));
        }
        return text;
    }

    DiagResult baud() const
    {
        const auto s = _snapshot();
        std::string text;
        line(text, "SIO UART now: set " + std::to_string(s.uart_baud_set) + ", hardware " + std::to_string(s.uart_baud_hw));
        line(text, "idx  POKEY PAL  POKEY NTSC  target   UART     vs PAL  vs NTSC  vs target");
        for (std::uint32_t i = 0; i <= max_baud_table_index; ++i) {
            const auto pal = pokey_baud(pal_clock_hz, i);
            const auto ntsc = pokey_baud(ntsc_clock_hz, i);
            const auto target = io::AtariSioSpeed::baud_for_index(i);
            const auto actual = _probe_baud(target);
            if (actual == 0) {
                line(text, std::to_string(i) + "    probe failed");
                continue;
            }
            std::string row = std::to_string(i);
            row.resize(5, ' ');
            for (const auto v : {pal, ntsc, target, actual}) {
                std::string col = std::to_string(v);
                col.resize(11, ' ');
                row += col;
            }
            line(text, row + error_pct(actual, pal) + "   " + error_pct(actual, ntsc) + "   " + error_pct(actual, target));
        }
        line(text, "target: legacy firmware baud (PAL clock, +3 cycles per byte). A few tenths of a percent is fine; "
                   "run with the Atari idle: this retunes the UART for a moment.");
        return DiagResult::ok(std::move(text));
    }

    // status() is split into small functions: one function with all the string temporaries needed about
    // 4.6 KB of stack, and the console task has 8 KB (a stack overflow in fujinet_core).
    DiagResult status() const
    {
        const io::AtariSioStats s = _snapshot();
        std::string text;
        status_link(s, text);
        status_frames(s, text);
        status_timing(s, text);
        status_data(s, text);
        status_commands(s, text);
        status_last(s, text);
        status_tail(s, text);
        return DiagResult::ok(std::move(text));
    }

    static std::string n(std::uint32_t v) { return std::to_string(v); }

    [[gnu::noinline]] void status_link(const io::AtariSioStats& s, std::string& text) const
    {
        line(text, "cmd_edges: " + n(s.cmd_edges) + "   cmd_line: " + (s.cmd_line_low ? "low (asserted)" : "high (idle)"));
        line(text, "baud: set " + n(s.uart_baud_set) + ", UART hardware reports " + n(s.uart_baud_hw));
        text += hsio_line(s);
        if (_set_proc) text += proc_line(s);
        if (_set_tuning) line(text, tuning_text(s.tuning));
        if (s.alt_baud != 0) {
            line(text, "second listener (UART2, same RX pin) at " + n(s.alt_baud) + " baud: command frames decoded " +
                           n(s.alt_valid_frames) + ", used instead of the main UART's " +
                           n(s.capture.frames_from_second_listener) + (s.alt_valid_frames ? ", last " + frame_text(s.alt_last_frame) : ""));
            line(text, "second listener frames discarded with ambiguous CMD edges: " + n(s.alt_ambiguous_dropped));
        }
        for (std::size_t i = 0; i < s.capture.first_alt_decisions.size() &&
                                i < s.capture.frames_from_second_listener; ++i) {
            const auto& d = s.capture.first_alt_decisions[i];
            line(text, "  alt decision " + n(static_cast<std::uint32_t>(i + 1)) + ": " + frame_text(d.frame) +
                       "; CMD edges " + n(d.edges) + ", collecting " + n(d.collecting) +
                       ", released " + n(d.released) + ", main bytes already collected " + n(d.main_bytes));
        }
    }

    [[gnu::noinline]] void status_frames(const io::AtariSioStats& s, std::string& text) const
    {
        line(text, "rx_bytes: " + n(s.capture.rx_bytes) + "   uart_rx_errors: " + n(s.uart_rx_errors));
        line(text, "frames: " + n(s.capture.frames) + "   disk(D1:-D8:): " + n(s.router.disk_commands) +
                       "   nio(0x7f): " + n(s.router.nio_commands) + "   other devices: " + n(s.router.other_commands));
        line(text, "aborts: edge=" + n(s.capture.aborts_edge) + " timeout=" + n(s.capture.aborts_timeout) +
                       " overflow=" + n(s.capture.aborts_overflow) + " rx_error=" + n(s.capture.aborts_rx_error) +
                       "   (edge aborts while a write waited for its data: " + n(s.capture.aborts_edge_in_write) +
                       "; CMD edges ignored there: " + n(s.capture.write_glitches_ignored) + ")");
        line(text, "command frames lost (abandoned while being received, so the Atari retried): " +
                       n(s.capture.frames_lost));
    }

    [[gnu::noinline]] void status_timing(const io::AtariSioStats& s, std::string& text) const
    {
        const auto& r = s.reply;
        line(text, "boot (ms): ingress ready " + at_ms(r.ready_us) + "   first CMD edge " + at_ms(s.first_edge_us) +
                       "   first frame " + at_ms(s.capture.first_frame_us));
        line(text, "react: CMD release -> start of processing, max " + ms1(s.capture.lag_us_max));
        line(text, "reply: CMD release -> first byte, last " + ms1(r.reply_us_last) + ", max " + ms1(r.reply_us_max) +
                       ", over 16 ms: " + n(r.reply_over_deadline) + "   (replies " + n(r.replies) + ")");
        line(text, "poll gap: longest time between polls (not counting serving a command) while CMD was active, " + ms1(s.poll_gap_us_max) +
                       (s.poll_gap_us_max ? ", starting at " + at_ms(s.poll_gap_at_us) + " ms" : ""));
        if (s.tx_noise_edges != 0)
            line(text, "CMD edges while we transmitted, dropped as noise: " + n(s.tx_noise_edges));
        if (s.stale_replies_dropped != 0)
            line(text, "native replies never read, discarded: " + n(s.stale_replies_dropped));
        line(text, "handler: last " + ms1(r.handle_us_last) + ", max " + ms1(r.handle_us_max));
    }

    [[gnu::noinline]] void status_data(const io::AtariSioStats& s, std::string& text) const
    {
        const auto& r = s.reply;
        if (r.data_replies != 0 && r.data_bytes_total != 0) {
            // Tenths of a microsecond per byte, so the figure is readable without floating point.
            const auto tenths = [](std::uint64_t us, std::uint64_t bytes) {
                const auto t = us * 10 / bytes;
                return std::to_string(t / 10) + "." + std::to_string(t % 10);
            };
            std::string data = "data frames (over 64 bytes): " + n(r.data_replies) + " sent, " +
                               tenths(r.data_us_total, r.data_bytes_total) + " us/byte on average";
            if (s.uart_baud_set != 0) data += " (the wire: " + tenths(10'000'000ull, s.uart_baud_set) + ")";
            data += "; last " + n(r.data_bytes_last) + " bytes in " + ms1(r.data_us_last);
            line(text, data);
        }
    }

    [[gnu::noinline]] void status_commands(const io::AtariSioStats& s, std::string& text) const
    {
        {
            // Per command: how many, and the average time we took (service) and then the Atari took
            // before its next command (idle). Their sum over all commands is the elapsed time.
            static const char* const names[io::AtariSioTxnStats::buckets] = {"Q", "W", "P", "R", "other"};
            std::string txn;
            for (std::size_t i = 0; i < io::AtariSioTxnStats::buckets; ++i) {
                const auto& e = s.txn.entry[i];
                if (e.count == 0) continue;
                const auto avg = [&](std::uint64_t total) {
                    const auto t = total * 10 / e.count / 1000;   // tenths of a ms
                    return std::to_string(t / 10) + "." + std::to_string(t % 10);
                };
                txn += std::string(txn.empty() ? "" : "; ") + names[i] + " x" + n(e.count) + " service " +
                       avg(e.service_us) + " ms, idle after " + avg(e.idle_us) + " ms";
            }
            if (!txn.empty()) line(text, "per command (FujiNet time, then the Atari's until its next command): " + txn);
        }
    }

    [[gnu::noinline]] void status_last(const io::AtariSioStats& s, std::string& text) const
    {
        if (s.router.has_last_frame) {
            const auto& f = s.router.last_frame;
            std::string last = "last_frame: ";
            for (std::size_t i = 0; i < f.size(); ++i) {
                last += (i ? " " : "") + hex2(f[i]);
            }
            last += "   device " + hex2(f[0]) + ", command " + hex2(f[1]);
            if (const char* name = command_name(f[0], f[1])) {
                last += std::string(" (") + name + ")";
            }
            line(text, last);
        } else {
            line(text, "last_frame: none yet");
        }
        if (s.router.refused != 0) {
            line(text, "refused (answered NAK, for our devices): " + n(s.router.refused) + "   first " +
                           frame_text(s.router.first_refused) + "   last " + frame_text(s.router.last_refused));
        }
        for (std::size_t i = 0; i < s.router.other_first_count; ++i) {
            line(text, "other device, frame " + std::to_string(i + 1) + ": " + frame_text(s.router.other_first[i]));
        }
        if (s.router.has_last_other) line(text, "last frame for another device: " + frame_text(s.router.last_other));
    }

    [[gnu::noinline]] void status_tail(const io::AtariSioStats& s, std::string& text) const
    {
        line(text, "reading: " + reading(s));
        for (const auto& w : watch(s)) {
            line(text, "watch: " + w);
        }
    }

    // Things worth a look even when the signal path is fine: timing that endangers a reply.
    static std::vector<std::string> watch(const io::AtariSioStats& s)
    {
        std::vector<std::string> out;
        const auto& r = s.reply;
        if (r.reply_over_deadline != 0) {
            out.push_back(std::to_string(r.reply_over_deadline) + " command reply(ies) started more than 16 ms after "
                          "CMD was released (worst " + ms1(r.reply_us_max) + "): the loop was busy; see react and handler");
        }
        if (r.handle_us_max >= slow_handler_us) {
            out.push_back("a request spent " + ms1(r.handle_us_max) + " in its handler: slow work on the SIO task "
                          "stalls every reply, and the Atari times out and leaves the native endpoint stuck");
        }
        if (s.poll_gap_us_max >= slow_handler_us) {
            out.push_back("the SIO task went " + ms1(s.poll_gap_us_max) + " without polling while CMD was active: "
                          "something blocked it (a slow handler); commands sent meanwhile may be missed");
        }
        if (s.first_edge_us != 0 && r.ready_us != 0 && s.first_edge_us <= r.ready_us) {
            out.push_back("the first CMD edge came before the ingress was ready: early commands may have been missed");
        }
        return out;
    }

    static constexpr std::uint32_t slow_handler_us = 100000;

    // The first thing that is wrong, in the order the signal travels.
    static std::string reading(const io::AtariSioStats& s)
    {
        if (s.cmd_edges == 0) {
            return "the CMD line has not changed since the last reset: the Atari is not sending commands, "
                   "or CMD is on the wrong pin or not connected";
        }
        if (s.capture.rx_bytes == 0) {
            return "CMD toggles but no bytes arrive on the SIO data-in pin: check that pin and the cable";
        }
        if (s.capture.frames == 0) {
            return "bytes arrive but no complete command frame: see the aborts (edge: CMD timing, "
                   "rx_error: baud or wiring, timeout/overflow: framing)";
        }
        if (s.router.disk_commands == 0 && s.router.nio_commands == 0) {
            return "command frames arrive, but none are for D1:-D8: or 0x7f (other devices only)";
        }
        if (s.router.disk_commands != 0) {
            return "disk commands reach the disk frontend; if the Atari still does not boot, "
                   "check the replies (the data-out pin) and disk.stats";
        }
        return "native (0x7f) commands reach the endpoint";
    }

    std::function<io::AtariSioStats()> _snapshot;
    std::function<void()> _reset;
    std::function<std::uint32_t(std::uint32_t)> _probe_baud;
    std::function<bool(int, bool)> _set_hsio_index;
    std::function<void(const io::AtariSioTuning&)> _set_tuning;
    std::function<std::string(int, const io::AtariSioTuning&)> _save;
    std::function<bool(bool)> _set_proc;
    std::function<bool(unsigned, unsigned, unsigned)> _pulse_proc;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_atari_sio_diagnostic_provider(
    std::function<io::AtariSioStats()> snapshot,
    std::function<void()> reset,
    std::function<std::uint32_t(std::uint32_t)> probe_baud,
    std::function<bool(int, bool)> set_hsio_index,
    std::function<void(const io::AtariSioTuning&)> set_tuning,
    std::function<std::string(int, const io::AtariSioTuning&)> save,
    std::function<bool(bool)> set_proc,
    std::function<bool(unsigned, unsigned, unsigned)> pulse_proc)
{
    if (!snapshot || !reset) {
        return nullptr;
    }
    return std::make_unique<AtariSioDiagnosticProvider>(std::move(snapshot), std::move(reset), std::move(probe_baud),
                                                        std::move(set_hsio_index), std::move(set_tuning), std::move(save),
                                                        std::move(set_proc), std::move(pulse_proc));
}

} // namespace fujinet::diag

#endif // FN_BUILD_ATARI_FUJIBUS_SIO || FN_PLATFORM_POSIX
