
#pragma once

#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

std::unique_ptr<IFileSystem> make_http_filesystem();

} // namespace fujinet::fs
