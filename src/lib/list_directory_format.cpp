#include "fujinet/io/list_directory_format.h"

#include "fujinet/platform/time.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace fujinet::io {
namespace {

std::string format_size_readable(std::uint64_t bytes)
{
    if (bytes == 0) {
        return "0";
    }

    const char* units[] = {"B", "K", "M", "G", "T"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }

    // Format to 1 decimal place for values with decimals, or no decimals for whole numbers
    char buffer[32];
    if (size == static_cast<int>(size)) {
        std::snprintf(buffer, sizeof(buffer), "%.0f%s", size, units[unit_index]);
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
    const std::string size_field = format_size_readable(entry.sizeBytes);
    const std::string date_field =
        format_mtime_ls(entry.modifiedTime);

    std::string name(basename);
    std::string line;
    line.reserve(100); // Reasonable estimate for line length
    line.push_back(type_ch);
    line.push_back(' ');
    line.append(size_field);
    line.append("  ");
    line.append(date_field);
    line.push_back(' ');
    line.append(name);
    line.push_back('\n');
    return line;
}

} // namespace fujinet::io

