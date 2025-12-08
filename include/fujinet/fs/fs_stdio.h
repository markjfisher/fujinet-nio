#pragma once

#include <memory>
#include <string>
#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

// Generic stdio-backed filesystem: works on POSIX and ESP-IDF VFS.
std::unique_ptr<IFileSystem>
create_stdio_filesystem(const std::string& rootDir,
                        const std::string& name,
                        FileSystemKind kind);

} // namespace fujinet::fs
