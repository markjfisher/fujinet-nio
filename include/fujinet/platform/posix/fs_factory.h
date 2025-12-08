#pragma once

#include <string>
#include "fujinet/fs/filesystem.h"

namespace fujinet::platform::posix {

std::unique_ptr<fujinet::fs::IFileSystem>
create_host_filesystem(const std::string& rootDir);

} // namespace fujinet::platform::posix