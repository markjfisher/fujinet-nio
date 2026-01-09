#include "fujinet/console/console_engine.h"

#include "fujinet/console/console_commands.h"
#include "fujinet/console/console_parse.h"
#include "fujinet/console/fs_shell.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/platform/time.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::console {

namespace {

static const char* status_to_str(diag::DiagStatus st)
{
    using diag::DiagStatus;
    switch (st) {
    case DiagStatus::Ok:         return "ok";
    case DiagStatus::Error:      return "error";
    case DiagStatus::NotFound:   return "not_found";
    case DiagStatus::InvalidArgs:return "invalid_args";
    case DiagStatus::NotReady:   return "not_ready";
    case DiagStatus::Busy:       return "busy";
    }
    return "unknown";
}

static void print_diagnostic_help(diag::DiagnosticRegistry& reg, IConsoleTransport& io)
{
    std::vector<diag::DiagCommandSpec> cmds;
    reg.list_all_commands(cmds);

    io.write_line("");
    io.write_line("diagnostics:");

    if (cmds.empty()) {
        io.write_line("  (no diagnostics registered)");
        return;
    }

    std::sort(cmds.begin(), cmds.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });

    auto provider_of = [](std::string_view full) -> std::string_view {
        const std::size_t dot = full.find('.');
        if (dot == std::string_view::npos || dot == 0) return "misc";
        return full.substr(0, dot);
    };

    std::string_view cur;
    for (const auto& c : cmds) {
        const std::string_view prov = provider_of(c.name);
        if (prov != cur) {
            cur = prov;
            io.write_line("");
            io.write("  [");
            io.write(cur);
            io.write_line("]");
        }
        io.write("    ");
        io.write(c.name);
        if (!c.summary.empty()) {
            io.write(" - ");
            io.write(c.summary);
        }
        io.write_line("");
    }
}

} // namespace

ConsoleEngine::ConsoleEngine(diag::DiagnosticRegistry& registry, IConsoleTransport& io)
    : _registry(registry)
    , _io(io)
{
    _cmds = std::make_unique<ConsoleCommandRegistry>();

    // Core console commands
    (void)_cmds->register_command(ConsoleCommandSpec{
        .name = "help",
        .summary = "show this help",
        .usage = "help",
    }, [this](const auto&) {
        _cmds->print_help(_io);
        print_diagnostic_help(_registry, _io);
        return true;
    });

    (void)_cmds->register_command(ConsoleCommandSpec{
        .name = "kill",
        .summary = "terminate fujinet-nio (stops the process)",
        .usage = "kill",
    }, [this](const auto&) {
        _io.write_line("warning: stopping fujinet-nio");
        return false;
    });

    (void)_cmds->register_command(ConsoleCommandSpec{
        .name = "reboot",
        .summary = "reboot/reset via platform hook (if supported)",
        .usage = "reboot",
    }, [this](const auto&) {
        if (_reboot) {
            _io.write_line("reboot: requested");
            _reboot();
        } else {
            _io.write_line("error: reboot not supported on this platform/app");
        }
        return true;
    });

    // Start "disconnected" so the first observed connected state prints MOTD/prompt.
    _last_connected = false;
}

ConsoleEngine::ConsoleEngine(diag::DiagnosticRegistry& registry, IConsoleTransport& io, fujinet::fs::StorageManager& storage)
    : _registry(registry)
    , _io(io)
    , _storage(&storage)
{
    _cmds = std::make_unique<ConsoleCommandRegistry>();

    // Core console commands
    (void)_cmds->register_command(ConsoleCommandSpec{
        .name = "help",
        .summary = "show this help",
        .usage = "help",
    }, [this](const auto&) {
        _cmds->print_help(_io);
        print_diagnostic_help(_registry, _io);
        return true;
    });

    (void)_cmds->register_command(ConsoleCommandSpec{
        .name = "kill",
        .summary = "terminate fujinet-nio (stops the process)",
        .usage = "kill",
    }, [this](const auto&) {
        _io.write_line("warning: stopping fujinet-nio");
        return false;
    });

    (void)_cmds->register_command(ConsoleCommandSpec{
        .name = "reboot",
        .summary = "reboot/reset via platform hook (if supported)",
        .usage = "reboot",
    }, [this](const auto&) {
        if (_reboot) {
            _io.write_line("reboot: requested");
            _reboot();
        } else {
            _io.write_line("error: reboot not supported on this platform/app");
        }
        return true;
    });

    // FS shell helper
    _fs_shell = std::make_unique<FsShell>(storage);
    (void)_fs_shell->register_commands(*_cmds, _io);

    // Start "disconnected" so the first observed connected state prints MOTD/prompt.
    _last_connected = false;
}

ConsoleEngine::~ConsoleEngine() = default;

void ConsoleEngine::run_loop()
{
    while (step(-1)) {
        // loop
    }
}

bool ConsoleEngine::step(int timeout_ms)
{
    const bool connected = _io.is_connected();
    if (connected && !_last_connected) {
        // New connection: print MOTD + prompt.
        _io.write_line("fujinet-nio diagnostic console (type: help)");
        _edit_rendered = false;
        // Render initial prompt without ANSI clears (matches read_line_edit first render behavior).
        _io.write(_prompt);
        _io.write(_edit);
        _edit_rendered = true;
    }
    if (!connected) {
        // While disconnected, avoid emitting prompts (they'll just be lost).
        _last_connected = connected;
        return true;
    }
    _last_connected = connected;

    std::string line;
    if (!read_line_edit(line, timeout_ms)) {
        return true; // no input / timeout
    }
    return handle_line(line);
}

void ConsoleEngine::clear_edit_line()
{
    // Avoid '\r' here: many serial clients map CR->LF (or CRLF), which would
    // make every redraw jump to the next line. Use ANSI "cursor to column 1".
    _io.write("\x1b[2K\x1b[1G");
}

void ConsoleEngine::render_edit_line()
{
    clear_edit_line();
    _io.write(_prompt);
    _io.write(_edit);
    _io.write("\x1b[K"); // clear to end

    const std::size_t right = (_edit.size() >= _cursor) ? (_edit.size() - _cursor) : 0;
    if (right != 0) {
        _io.write("\x1b[");
        _io.write(std::to_string(right));
        _io.write("D"); // cursor left
    }
}

static bool is_word_char(char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

static std::size_t word_left(std::string_view s, std::size_t cur)
{
    if (cur == 0) return 0;
    std::size_t i = cur;
    while (i > 0 && !is_word_char(s[i - 1])) --i;
    while (i > 0 && is_word_char(s[i - 1])) --i;
    return i;
}

static std::size_t word_right(std::string_view s, std::size_t cur)
{
    std::size_t i = cur;
    while (i < s.size() && !is_word_char(s[i])) ++i;
    while (i < s.size() && is_word_char(s[i])) ++i;
    return i;
}

bool ConsoleEngine::read_line_edit(std::string& out_line, int timeout_ms)
{
    out_line.clear();

    if (!_edit_rendered) {
        // First prompt render: avoid ANSI clear codes until we actually need to redraw.
        // Some serial clients (or mappings) will show ESC sequences visibly.
        _io.write(_prompt);
        _io.write(_edit);
        _edit_rendered = true;
    }

    auto apply_history = [&](std::size_t idx) {
        if (idx >= _history.size()) return;
        _edit = _history[idx];
        _cursor = _edit.size();
        render_edit_line();
    };

    auto history_up = [&]() {
        if (_history.empty()) return;
        if (!_hist_active) {
            _hist_active = true;
            _hist_saved = _edit;
            _hist_index = _history.size() - 1;
        } else if (_hist_index > 0) {
            --_hist_index;
        }
        apply_history(_hist_index);
    };

    auto history_down = [&]() {
        if (!_hist_active) return;
        if (_hist_index + 1 < _history.size()) {
            ++_hist_index;
            apply_history(_hist_index);
            return;
        }
        _hist_active = false;
        _edit = _hist_saved;
        _cursor = _edit.size();
        render_edit_line();
    };

    auto commit_line = [&]() {
        _io.write("\r\n");
        out_line = _edit;
        out_line = std::string(trim_ws(out_line));

        if (!out_line.empty()) {
            if (_history.empty() || _history.back() != out_line) {
                _history.push_back(out_line);
                if (_history.size() > _history_max) {
                    _history.erase(_history.begin());
                }
            }
        }

        _edit.clear();
        _cursor = 0;
        _esc.clear();
        _pending_eol = 0;
        _hist_active = false;
        _hist_saved.clear();
        _edit_rendered = false;
    };

    auto handle_escape = [&](const std::string& esc) -> bool {
        // Arrow keys / home/end (common xterm sequences).
        if (esc == "\x1b[A") { history_up(); return true; }
        if (esc == "\x1b[B") { history_down(); return true; }
        if (esc == "\x1b[C") { if (_cursor < _edit.size()) { ++_cursor; render_edit_line(); } return true; }
        if (esc == "\x1b[D") { if (_cursor > 0) { --_cursor; render_edit_line(); } return true; }

        // Home/End variants.
        if (esc == "\x1b[H" || esc == "\x1bOH" || esc == "\x1b[1~") { _cursor = 0; render_edit_line(); return true; }
        if (esc == "\x1b[F" || esc == "\x1bOF" || esc == "\x1b[4~") { _cursor = _edit.size(); render_edit_line(); return true; }

        // Delete (forward delete)
        if (esc == "\x1b[3~") {
            if (_cursor < _edit.size()) {
                _edit.erase(_cursor, 1);
                render_edit_line();
            }
            return true;
        }

        // Alt-b / Alt-f (word move) - common: ESC b / ESC f
        if (esc.size() == 2 && esc[0] == '\x1b' && esc[1] == 'b') { _cursor = word_left(_edit, _cursor); render_edit_line(); return true; }
        if (esc.size() == 2 && esc[0] == '\x1b' && esc[1] == 'f') { _cursor = word_right(_edit, _cursor); render_edit_line(); return true; }

        // xterm alt-left/right: ESC [ 1 ; 3 D/C
        if (esc == "\x1b[1;3D") { _cursor = word_left(_edit, _cursor); render_edit_line(); return true; }
        if (esc == "\x1b[1;3C") { _cursor = word_right(_edit, _cursor); render_edit_line(); return true; }

        return false;
    };

    auto process_byte = [&](std::uint8_t b) -> bool {
        // Returns true when a line was committed.

        // Swallow CRLF/LFCR pairs as a single "enter".
        if (_pending_eol != 0) {
            const bool is_pair = (_pending_eol == '\r' && b == '\n') || (_pending_eol == '\n' && b == '\r');
            _pending_eol = 0;
            if (is_pair) {
                return false;
            }
        }

        if (!_esc.empty() || b == 0x1b) {
            if (_esc.empty() && b == 0x1b) {
                _esc.push_back(static_cast<char>(b));
                return false;
            }
            if (!_esc.empty()) {
                _esc.push_back(static_cast<char>(b));

                // ESC <letter> (alt bindings) can complete at len==2.
                if (_esc.size() == 2 && (_esc[1] == 'b' || _esc[1] == 'f')) {
                    (void)handle_escape(_esc);
                    _esc.clear();
                    return false;
                }

                // CSI / SS3 sequences complete on a final alpha, or '~'.
                const char last = _esc.back();
                if (last == '~' || (last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z')) {
                    (void)handle_escape(_esc);
                    _esc.clear();
                    return false;
                }

                // Safety: drop unknown/long sequences.
                if (_esc.size() > 8) {
                    _esc.clear();
                }
                return false;
            }
        }

        // Enter
        if (b == '\r' || b == '\n') {
            _pending_eol = static_cast<char>(b);
            commit_line();
            return true;
        }

        // Backspace / DEL (treat as backspace)
        if (b == 0x08 || b == 0x7f) {
            if (_cursor > 0) {
                _edit.erase(_cursor - 1, 1);
                --_cursor;
                render_edit_line();
            }
            return false;
        }

        // Ctrl-A / Ctrl-E
        if (b == 0x01) { _cursor = 0; render_edit_line(); return false; }
        if (b == 0x05) { _cursor = _edit.size(); render_edit_line(); return false; }

        // Ctrl-U: kill line
        if (b == 0x15) {
            _edit.clear();
            _cursor = 0;
            render_edit_line();
            return false;
        }

        // Ctrl-K: kill to end
        if (b == 0x0b) {
            if (_cursor < _edit.size()) {
                _edit.erase(_cursor);
                render_edit_line();
            }
            return false;
        }

        // Ctrl-W: delete word back
        if (b == 0x17) {
            const std::size_t nl = word_left(_edit, _cursor);
            if (nl < _cursor) {
                _edit.erase(nl, _cursor - nl);
                _cursor = nl;
                render_edit_line();
            }
            return false;
        }

        // Ctrl-P / Ctrl-N (history up/down, like emacs)
        if (b == 0x10) { history_up(); return false; }
        if (b == 0x0e) { history_down(); return false; }

        // Printable ASCII
        if (b >= 0x20 && b < 0x7f) {
            const char ch = static_cast<char>(b);
            if (_cursor == _edit.size()) {
                _edit.push_back(ch);
                ++_cursor;
            } else {
                _edit.insert(_cursor, 1, ch);
                ++_cursor;
            }
            render_edit_line();
            return false;
        }

        // Ignore other control bytes.
        return false;
    };

    std::uint8_t b = 0;
    if (!_io.read_byte(b, timeout_ms)) {
        return false;
    }

    if (process_byte(b)) {
        return true;
    }

    // Drain any immediately available bytes to keep escape sequences responsive.
    while (_io.read_byte(b, 0)) {
        if (process_byte(b)) {
            return true;
        }
    }

    return false;
}

static std::string fmt_time_ls(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{}) {
        return "??? ?? ??:??";
    }

    char buf[16];
    const std::uint64_t secs = static_cast<std::uint64_t>(std::chrono::system_clock::to_time_t(tp));
    if (!fujinet::platform::format_time_utc_ls(secs, buf, sizeof(buf))) {
        return "??? ?? ??:??";
    }
    return std::string(buf);
}

static std::string fmt_size(std::uint64_t bytes)
{
    // Fixed-ish width, human-ish (base-1024). Examples:
    //   314 -> "314"
    //   5271 -> "5.1K"
    //   102400 -> "100K"
    char buf[16];
    if (bytes < 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(bytes));
        return std::string(buf);
    }
    if (bytes < 1024ULL * 1024ULL) {
        const double k = static_cast<double>(bytes) / 1024.0;
        if (k >= 100.0) {
            std::snprintf(buf, sizeof(buf), "%.0fK", k);
        } else if (k >= 10.0) {
            std::snprintf(buf, sizeof(buf), "%.1fK", k);
        } else {
            std::snprintf(buf, sizeof(buf), "%.2fK", k);
        }
        return std::string(buf);
    }
    if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        const double m = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (m >= 100.0) {
            std::snprintf(buf, sizeof(buf), "%.0fM", m);
        } else if (m >= 10.0) {
            std::snprintf(buf, sizeof(buf), "%.1fM", m);
        } else {
            std::snprintf(buf, sizeof(buf), "%.2fM", m);
        }
        return std::string(buf);
    }
    const double g = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (g >= 100.0) {
        std::snprintf(buf, sizeof(buf), "%.0fG", g);
    } else if (g >= 10.0) {
        std::snprintf(buf, sizeof(buf), "%.1fG", g);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2fG", g);
    }
    return std::string(buf);
}

static std::string pad_left(std::string_view s, std::size_t width)
{
    if (s.size() >= width) return std::string(s);
    std::string out;
    out.reserve(width);
    out.append(width - s.size(), ' ');
    out.append(s.data(), s.size());
    return out;
}

bool ConsoleEngine::handle_line(std::string_view line)
{
    line = trim_ws(line);
    if (line.empty()) {
        return true;
    }

    // Ignore lines that contain ANSI escape (often init strings / terminal noise).
    if (line.find('\x1b') != std::string_view::npos) {
        return true;
    }

    const auto argv = split_ws(line);
    if (argv.empty()) {
        return true;
    }

    const std::string_view cmd0 = argv[0];
    // Try console-registered commands first.
    if (_cmds) {
        const auto r = _cmds->dispatch(argv);
        if (r.has_value()) {
            if (!r.value()) {
                _io.write_line("bye");
            }
            return r.value();
        }
    }

    // Default: dispatch as a diagnostic command:
    // - "net.sessions" (argv[0] already qualified)
    // - "net sessions ..." (argv[0]=provider, argv[1]=command)
    std::string dispatch_line;
    dispatch_line.reserve(64);

    struct Range { std::size_t off; std::size_t len; };
    std::vector<Range> ranges;

    if (cmd0.find('.') != std::string_view::npos) {
        dispatch_line.append(cmd0.data(), cmd0.size());
        ranges.push_back(Range{0, dispatch_line.size()});
        for (std::size_t i = 1; i < argv.size(); ++i) {
            dispatch_line.push_back(' ');
            const std::size_t off = dispatch_line.size();
            dispatch_line.append(argv[i].data(), argv[i].size());
            ranges.push_back(Range{off, argv[i].size()});
        }
    } else {
        if (argv.size() < 2) {
            _io.write_line("error: expected '<provider> <command>' (try: help)");
            return true;
        }

        dispatch_line.append(argv[0].data(), argv[0].size());
        dispatch_line.push_back('.');
        dispatch_line.append(argv[1].data(), argv[1].size());
        ranges.push_back(Range{0, dispatch_line.size()});
        for (std::size_t i = 2; i < argv.size(); ++i) {
            dispatch_line.push_back(' ');
            const std::size_t off = dispatch_line.size();
            dispatch_line.append(argv[i].data(), argv[i].size());
            ranges.push_back(Range{off, argv[i].size()});
        }
    }

    diag::DiagArgsView av;
    av.line = dispatch_line;
    av.argv.reserve(ranges.size());
    for (const auto& r : ranges) {
        av.argv.push_back(av.line.substr(r.off, r.len));
    }

    diag::DiagResult r = _registry.dispatch(av);
    if (r.status == diag::DiagStatus::NotFound) {
        _io.write_line("error: unknown command (try: help)");
        return true;
    }

    _io.write("status: ");
    _io.write_line(status_to_str(r.status));

    if (!r.text.empty()) {
        _io.write_line(r.text);
    }
    else if (!r.kv.empty()) {
        for (const auto& kv : r.kv) {
            _io.write(kv.first);
            _io.write(": ");
            _io.write_line(kv.second);
        }
    }

    return true;
}

} // namespace fujinet::console


