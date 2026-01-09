#include "fujinet/console/console_parse.h"

#include <cctype>

namespace fujinet::console {

std::string_view trim_ws(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::vector<std::string_view> split_ws(std::string_view s)
{
    std::vector<std::string_view> out;
    s = trim_ws(s);
    while (!s.empty()) {
        std::size_t i = 0;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        out.push_back(s.substr(0, i));
        s.remove_prefix(i);
        s = trim_ws(s);
    }
    return out;
}

} // namespace fujinet::console


