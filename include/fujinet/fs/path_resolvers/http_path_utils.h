#pragma once

#include <string>
#include <string_view>

namespace fujinet::fs {

bool is_http_uri(std::string_view spec);
std::string http_join_relative(std::string_view base, std::string_view rel);
std::string http_replace_path(std::string_view base, std::string_view absolutePath);

} // namespace fujinet::fs
