#pragma once

#include <string>
#include "fujinet/fs/filesystem.h"

namespace fujinet::platform::posix {

std::unique_ptr<fujinet::fs::IFileSystem>
create_host_filesystem(const std::string& rootDir);

std::unique_ptr<fujinet::fs::IFileSystem>
create_tnfs_filesystem(const std::string& host,
                       uint16_t port,
                       const std::string& mountPath = "/",
                       const std::string& user = "",
                       const std::string& password = "");

} // namespace fujinet::platform::posix