#pragma once

#include <string_view>
#include <vector>

namespace fujinet::console {

std::string_view trim_ws(std::string_view s);

// Split on ASCII whitespace, after trimming ends.
std::vector<std::string_view> split_ws(std::string_view s);

} // namespace fujinet::console


