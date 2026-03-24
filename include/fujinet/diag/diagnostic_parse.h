#pragma once

#include <cstdint>
#include <string_view>

namespace fujinet::diag {

/// Parse a non-empty decimal string into `out`. Rejects overflow and non-digits.
bool parse_decimal_u32(std::string_view s, std::uint32_t& out) noexcept;

/// ASCII-only, locale-independent case-insensitive equality.
bool ascii_iequals(std::string_view a, std::string_view b) noexcept;

} // namespace fujinet::diag
