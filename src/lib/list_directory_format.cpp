#include "fujinet/io/list_directory_format.h"

#include "fujinet/platform/time.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace fujinet::io {
namespace {

constexpr std::size_t kSizeFieldWidth = 7;  // e.g. "1000.0G"
constexpr std::size_t kDateFieldWidth = 12;

std::string pad_left(std::string_view value, std::size_t width)
{
    if (value.size() >= width) {
        return std::string(value.substr(0, width));
    }
    return std::string(width - value.size(), ' ') + std::string(value);
}

std::string format_size_readable(std::uint64_t bytes)
{
    if (bytes == 0) {
        return "0";
    }

    const char* units[] = {"B", "K", "M", "G", "T"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    // Scale like ls -h: divide by 1024 while >= 1024, or roll up at 1000 in the
    // current unit (so 1000M becomes 1.0G instead of staying at 1000M).
    while (unit_index < 4 && (size >= 1024.0 || size >= 1000.0)) {
        size /= 1024.0;
        ++unit_index;
    }

    char buffer[32];
    if (unit_index == 0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f%s", size, units[0]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f%s", size, units[unit_index]);
    }

    return std::string(buffer);
}
std::string format_mtime_ls(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{}) {
        return "??? ?? ??:??";
    }

    const std::uint64_t secs =
        static_cast<std::uint64_t>(std::chrono::system_clock::to_time_t(tp));
    char buf[16];
    if (!fujinet::platform::format_time_local_ls(secs, buf, sizeof(buf))) {
        return "??? ?? ??:??";
    }
    return std::string(buf);
}

} // namespace

std::string format_list_directory_line(
    const fujinet::fs::FileInfo& entry,
    std::string_view basename)
{
    const char type_ch = entry.isDirectory ? 'd' : '-';
    const std::string size_field =
        pad_left(format_size_readable(entry.sizeBytes), kSizeFieldWidth);
    const std::string date_field =
        pad_left(format_mtime_ls(entry.modifiedTime), kDateFieldWidth);

    std::string name(basename);
    if (entry.isDirectory && (name.empty() || name.back() != '/')) {
        name.push_back('/');
    }
    std::string line;
    line.reserve(100); // Reasonable estimate for line length
    line.push_back(type_ch);
    line.push_back(' ');
    line.append(size_field);
    line.append(" ");
    line.append(date_field);
    line.push_back(' ');
    line.append(name);
    line.push_back('\n');
    return line;
}

} // namespace fujinet::io

