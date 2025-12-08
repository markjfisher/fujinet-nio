#pragma once

#include <memory>
#include <string>

#include "fujinet/fs/filesystem.h"

namespace fujinet::platform::posix {

// Create a POSIX-backed filesystem rooted at `rootDir` (host path),
// exposed under logical name `name` (e.g. "host").
std::unique_ptr<fs::IFileSystem>
create_host_filesystem(const std::string& rootDir, const std::string& name);

} // namespace fujinet::platform::posix
