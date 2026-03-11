#pragma once

#include <cstddef>
#include <cstdint>

namespace fujinet::core {

void format_hex_prefix(const std::uint8_t* buffer, std::size_t len, char* out, std::size_t out_len);

// Debug: hexdump bytes to log (16 per line, hex + ASCII). No-op when FN_DEBUG is not set.
// tag: log tag for FN_LOGI; max_bytes: cap at 256 by default.
void log_hexdump(const char* tag, const std::uint8_t* data, std::size_t size, std::size_t max_bytes = 512);

}  // namespace fujinet::core
