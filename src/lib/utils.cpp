#include "fujinet/core/utils.h"
#include "fujinet/core/logging.h"

#include <cctype>
#include <cstdio>

namespace fujinet::core {

void log_hexdump(const char* tag, const std::uint8_t* data, std::size_t size, std::size_t max_bytes)
{
#if defined(FN_DEBUG)
    if (tag == nullptr || data == nullptr || size == 0) return;
    const std::size_t limit = (size > max_bytes) ? max_bytes : size;
    for (std::size_t off = 0; off < limit; off += 16) {
        char hex[16 * 3 + 1];
        char ascii[17];
        std::size_t n = (off + 16 <= limit) ? 16 : (limit - off);
        char* h = hex;
        for (std::size_t i = 0; i < n; ++i) {
            int w = std::snprintf(h, sizeof(hex) - static_cast<std::size_t>(h - hex), "%02x ", data[off + i]);
            if (w > 0) h += w;
        }
        *h = '\0';
        for (std::size_t i = 0; i < n; ++i) {
            std::uint8_t b = data[off + i];
            ascii[i] = (std::isprint(static_cast<unsigned char>(b)) != 0) ? static_cast<char>(b) : '.';
        }
        ascii[n] = '\0';
        FN_LOGI(tag, "  %04zx: %-48s |%s|", off, hex, ascii);
    }
    if (size > max_bytes) {
        FN_LOGI(tag, "  ... (%zu bytes total, truncated)", size);
    }
#else
    (void)tag;
    (void)data;
    (void)size;
    (void)max_bytes;
#endif
}

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
