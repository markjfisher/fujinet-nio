#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fujinet::console {

class IConsoleTransport;

struct ConsoleCommandSpec {
    std::string name;
    std::string summary;
    std::string usage;
};

// Command handler returns:
// - true  => continue console loop
// - false => exit console loop
using ConsoleCommandFn = std::function<bool(const std::vector<std::string_view>& argv)>;

class ConsoleCommandRegistry {
public:
    bool register_command(ConsoleCommandSpec spec, ConsoleCommandFn fn);

    // Returns:
    // - nullopt if no command matched
    // - true/false based on handler result
    std::optional<bool> dispatch(const std::vector<std::string_view>& argv) const;

    void list_commands(std::vector<ConsoleCommandSpec>& out) const;

    void print_help(IConsoleTransport& io) const;

private:
    struct Entry {
        ConsoleCommandSpec spec;
        ConsoleCommandFn fn;
    };

    std::unordered_map<std::string, Entry> _cmds;
};

} // namespace fujinet::console


