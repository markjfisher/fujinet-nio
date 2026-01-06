#include "fujinet/console/console_engine.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::console {

namespace {

static std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

static std::vector<std::string_view> split_ws(std::string_view s)
{
    std::vector<std::string_view> out;
    s = trim(s);
    while (!s.empty()) {
        std::size_t i = 0;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        out.push_back(s.substr(0, i));
        s.remove_prefix(i);
        s = trim(s);
    }
    return out;
}

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

} // namespace

ConsoleEngine::ConsoleEngine(diag::DiagnosticRegistry& registry, IConsoleTransport& io)
    : _registry(registry)
    , _io(io)
{}

void ConsoleEngine::run_loop()
{
    std::string line;
    for (;;) {
        line.clear();
        if (!_io.read_line(line, -1)) {
            break;
        }
        if (!handle_line(line)) {
            break;
        }
    }
}

bool ConsoleEngine::step(int timeout_ms)
{
    std::string line;

    if (!_promptShown) {
        _io.write("> ");
        _promptShown = true;
    }

    if (!_io.read_line(line, timeout_ms)) {
        return true; // timeout / no input
    }

    // Most PTY clients will not echo locally; start a fresh line for output.
    _io.write_line("");
    _promptShown = false;

    return handle_line(line);
}

bool ConsoleEngine::handle_line(std::string_view line)
{
    line = trim(line);
    if (line.empty()) {
        return true;
    }

    const auto argv = split_ws(line);
    if (argv.empty()) {
        return true;
    }

    const std::string_view cmd0 = argv[0];
    if (cmd0 == "exit" || cmd0 == "quit") {
        _io.write_line("bye");
        return false;
    }

    if (cmd0 == "help") {
        _io.write_line("commands:");
        _io.write_line("  help");
        _io.write_line("  exit | quit");
        _io.write_line("  list                  (list diagnostic commands)");
        _io.write_line("  dump                  (run all diagnostic commands)");
        _io.write_line("  <provider> <command> [args...]");
        _io.write_line("  <provider>.<command> [args...]");
        return true;
    }

    // Shorthand: list commands without needing "diag".
    if (cmd0 == "list") {
        std::vector<diag::DiagCommandSpec> cmds;
        _registry.list_all_commands(cmds);
        std::sort(cmds.begin(), cmds.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });

        if (cmds.empty()) {
            _io.write_line("(no diagnostics registered)");
            return true;
        }

        for (const auto& c : cmds) {
            _io.write(c.name);
            if (!c.summary.empty()) {
                _io.write(" - ");
                _io.write(c.summary);
            }
            _io.write_line("");
        }
        return true;
    }

    if (cmd0 == "dump") {
        std::vector<diag::DiagCommandSpec> cmds;
        _registry.list_all_commands(cmds);
        std::sort(cmds.begin(), cmds.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });

        std::size_t skipped_unsafe = 0;
        for (const auto& c : cmds) {
            if (!c.safe) {
                ++skipped_unsafe;
                continue;
            }

            diag::DiagArgsView av;
            av.line = c.name;
            av.argv.push_back(av.line);

            diag::DiagResult r = _registry.dispatch(av);

            _io.write("== ");
            _io.write(c.name);
            _io.write(" (");
            _io.write(status_to_str(r.status));
            _io.write_line(") ==");

            if (!r.text.empty()) {
                _io.write_line(r.text);
            }
        }

        if (skipped_unsafe != 0) {
            _io.write("skipped_unsafe: ");
            _io.write_line(std::to_string(skipped_unsafe));
        }

        return true;
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


