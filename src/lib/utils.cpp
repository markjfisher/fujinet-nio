#include "fujinet/core/utils.h"

#include <cstdio>

namespace fujinet::core {

void format_hex_prefix(const std::uint8_t* buffer, std::size_t len, char* out, std::size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    std::size_t pos = 0;
    out[0] = '\0';

    const std::size_t n = (len < 16) ? len : 16;
    for (std::size_t i = 0; i < n; ++i) {
        int wrote = std::snprintf(out + pos, out_len - pos, "%02X%s", buffer[i], (i + 1 == n) ? "" : " ");
        if (wrote <= 0) {
            break;
        }
        pos += static_cast<std::size_t>(wrote);
        if (pos >= out_len) {
            out[out_len - 1] = '\0';
            break;
        }
    }
}

}  // namespace fujinet::core
