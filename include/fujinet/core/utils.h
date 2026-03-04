#pragma once

#include <cstdint>
#include <cstddef>

namespace fujinet::core {

void format_hex_prefix(const std::uint8_t* buffer, std::size_t len, char* out, std::size_t out_len);

}  // namespace fujinet::core
