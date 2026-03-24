#include "fujinet/diag/diagnostic_parse.h"

#include <cctype>
#include <cstdint>

namespace fujinet::diag {

bool parse_decimal_u32(std::string_view s, std::uint32_t& out) noexcept
{
    if (s.empty()) {
        return false;
    }
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
        if (v > 0xFFFFFFFFull) {
            return false;
        }
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

bool ascii_iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

} // namespace fujinet::diag
