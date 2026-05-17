#include "fujinet/io/list_directory_format.h"

#include "fujinet/platform/time.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace fujinet::io {
namespace {

constexpr std::size_t kMinLineWidth = 20;
constexpr std::size_t kMaxLineWidth = 120;
constexpr std::size_t kSizeFieldWidth = 11;
constexpr std::size_t kDateFieldWidth = 12;
constexpr std::size_t kPrefixWidth = 2; // "d " or "- "

std::string format_size_commas(std::uint64_t bytes)
{
    char raw[32];
    std::snprintf(raw, sizeof(raw), "%llu", static_cast<unsigned long long>(bytes));
    std::string s(raw);
    const std::size_t first_group = s.size() % 3;
    std::string out;
    out.reserve(s.size() + s.size() / 3);
    std::size_t i = 0;
    if (first_group != 0) {
        out.append(s, 0, first_group);
        i = first_group;
        if (i < s.size()) {
            out.push_back(',');
        }
    }
    while (i < s.size()) {
        out.append(s, i, 3);
        i += 3;
        if (i < s.size()) {
            out.push_back(',');
        }
    }
    return out;
}

std::string pad_left(std::string_view value, std::size_t width)
{
    if (value.size() >= width) {
        return std::string(value.substr(0, width));
    }
    return std::string(width - value.size(), ' ') + std::string(value);
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
    std::string_view basename,
    std::uint8_t line_width)
{
    const std::size_t width =
        std::clamp<std::size_t>(line_width, kMinLineWidth, kMaxLineWidth);
    const std::size_t name_width =
        (width > kPrefixWidth + kSizeFieldWidth + 2 + kDateFieldWidth + 1)
            ? width - (kPrefixWidth + kSizeFieldWidth + 2 + kDateFieldWidth + 1)
            : 1;

    const char type_ch = entry.isDirectory ? 'd' : '-';
    const std::string size_field = pad_left(format_size_commas(entry.sizeBytes), kSizeFieldWidth);
    const std::string date_field =
        pad_left(format_mtime_ls(entry.modifiedTime), kDateFieldWidth);

    std::string name(basename);
    if (name.size() > name_width) {
        name.resize(name_width);
    }

    std::string line;
    line.reserve(width);
    line.push_back(type_ch);
    line.push_back(' ');
    line.append(size_field);
    line.append("  ");
    line.append(date_field);
    line.push_back(' ');
    line.append(name);
    if (line.size() < width) {
        line.append(width - line.size(), ' ');
    }
    return line;
}

} // namespace fujinet::io
