#include "fujinet/console/console_commands.h"

#include "fujinet/console/console_engine.h" // IConsoleTransport

#include <algorithm>

namespace fujinet::console {

bool ConsoleCommandRegistry::register_command(ConsoleCommandSpec spec, ConsoleCommandFn fn)
{
    if (spec.name.empty() || !fn) {
        return false;
    }
    if (_cmds.find(spec.name) != _cmds.end()) {
        return false;
    }
    Entry e;
    e.spec = std::move(spec);
    e.fn = std::move(fn);
    _cmds.emplace(e.spec.name, std::move(e));
    return true;
}

std::optional<bool> ConsoleCommandRegistry::dispatch(const std::vector<std::string_view>& argv) const
{
    if (argv.empty()) return std::nullopt;
    auto it = _cmds.find(std::string(argv[0]));
    if (it == _cmds.end()) return std::nullopt;
    return (it->second.fn)(argv);
}

void ConsoleCommandRegistry::list_commands(std::vector<ConsoleCommandSpec>& out) const
{
    out.clear();
    out.reserve(_cmds.size());
    for (const auto& kv : _cmds) {
        out.push_back(kv.second.spec);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
}

void ConsoleCommandRegistry::print_help(IConsoleTransport& io) const
{
    io.write_line("commands:");

    std::vector<ConsoleCommandSpec> cmds;
    list_commands(cmds);
    for (const auto& c : cmds) {
        io.write("  ");
        io.write(c.name);
        if (!c.summary.empty()) {
            io.write(" - ");
            io.write(c.summary);
        }
        io.write_line("");
    }
}

} // namespace fujinet::console


