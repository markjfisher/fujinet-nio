#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fujinet::fs {
class StorageManager;
}

namespace fujinet::console {

class IConsoleTransport;

// Small helper that implements a terminal-like filesystem shell on top of StorageManager.
// This is intentionally app-only / out-of-band: it does not go through device protocols.
//
// Commands handled:
// - fs
// - pwd
// - cd <fs:/>|<path>
// - ls [<fs:/>|<path>]
// - mkdir <path>
// - rmdir <path>
// - mv <from> <to>
//
// State:
// - current filesystem (cwd_fs)
// - current path within that filesystem (cwd_path)
class FsShell {
public:
    static bool handle(
        fujinet::fs::StorageManager& storage,
        IConsoleTransport& io,
        const std::vector<std::string_view>& argv,
        std::string& cwd_fs,
        std::string& cwd_path
    );
};

} // namespace fujinet::console


