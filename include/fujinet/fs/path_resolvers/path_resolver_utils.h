#pragma once

#include <string>
#include <string_view>

namespace fujinet::fs {

std::string fs_join(std::string_view base, std::string_view rel);
std::string fs_norm(std::string_view in);

} // namespace fujinet::fs
