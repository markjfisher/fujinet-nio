#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fujinet::fs {
class StorageManager;
}

namespace fujinet::console {

class IConsoleTransport;

class ConsoleCommandRegistry;

// Terminal-like filesystem shell commands, registered into the console command registry.
// This is intentionally app-only / out-of-band: it does not go through device protocols.
class FsShell {
public:
    explicit FsShell(fujinet::fs::StorageManager& storage)
        : _storage(storage)
    {}

    bool register_commands(ConsoleCommandRegistry& reg, IConsoleTransport& io);

    // Session state
    std::string& cwd_fs() noexcept { return _cwd_fs; }
    std::string& cwd_path() noexcept { return _cwd_path; }

private:
    bool cmd_fs(IConsoleTransport& io, const std::vector<std::string_view>& argv);
    bool cmd_pwd(IConsoleTransport& io, const std::vector<std::string_view>& argv);
    bool cmd_cd(IConsoleTransport& io, const std::vector<std::string_view>& argv);
    bool cmd_ls(IConsoleTransport& io, const std::vector<std::string_view>& argv);
    bool cmd_mkdir(IConsoleTransport& io, const std::vector<std::string_view>& argv);
    bool cmd_rm(IConsoleTransport& io, const std::vector<std::string_view>& argv);
    bool cmd_rmdir(IConsoleTransport& io, const std::vector<std::string_view>& argv);
    bool cmd_mv(IConsoleTransport& io, const std::vector<std::string_view>& argv);

    fujinet::fs::StorageManager& _storage;
    std::string _cwd_fs;
    std::string _cwd_path{"/"};
};

} // namespace fujinet::console


