#pragma once

#include "fujinet/fs/filesystem.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fujinet::io {

// Build one ls-style line: "d" or "-", size in human-readable format, date, name.
// Always terminates with '\n' and doesn't use fixed width.
std::string format_list_directory_line(
    const fujinet::fs::FileInfo& entry,
    std::string_view basename);

} // namespace fujinet::io
