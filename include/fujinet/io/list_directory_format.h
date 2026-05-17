#pragma once

#include "fujinet/fs/filesystem.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fujinet::io {

// Build one ls-style line: "d" or "-", size with thousands separators, date, name.
// line_width is the total line width (name truncated if needed).
std::string format_list_directory_line(
    const fujinet::fs::FileInfo& entry,
    std::string_view basename,
    std::uint8_t line_width);

} // namespace fujinet::io
