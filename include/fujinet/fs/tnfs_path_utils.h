#pragma once

#include <string>
#include <string_view>

namespace fujinet::fs {

bool is_tnfs_uri(std::string_view spec);
bool is_tnfs_endpoint_path(std::string_view p);
std::string tnfs_join_relative(std::string_view base, std::string_view rel);

} // namespace fujinet::fs
