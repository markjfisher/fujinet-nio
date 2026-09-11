#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io::protocol {

    enum class SlipByte : std::uint8_t {
        End     = 0xC0,
        Escape  = 0xDB,
        EscEnd  = 0xDC,
        EscEsc  = 0xDD,
    };

    // helper to remove noise of the cast
    constexpr std::uint8_t to_byte(SlipByte b) noexcept {
        return static_cast<std::uint8_t>(b);
    }

    using ByteBuffer = std::vector<std::uint8_t>;

// Pure SLIP helpers shared by framers and legacy FujiBus serial wrappers.
// Decode starts at the first END and retains legacy malformed-escape handling;
// callers own frame extraction and any protocol validation.
inline ByteBuffer decodeSLIP(const ByteBuffer& input)
{
    ByteBuffer output;
    output.reserve(input.size());  // worst case, same size

    const std::size_t len = input.size();
    std::size_t idx = 0;

    // Find the first SLIP_END
    while (idx < len && input[idx] != to_byte(SlipByte::End)) {
        ++idx;
    }

    if (idx == len) {
        // no frame start found
        return {};
    }

    // Decode from the byte after the first SLIP_END
    for (++idx; idx < len; ++idx) {
        std::uint8_t val = input[idx];
        if (val == to_byte(SlipByte::End)) {
            break;
        }

        if (val == to_byte(SlipByte::Escape)) {
            if (++idx >= len) {
                break; // truncated escape
            }
            val = input[idx];
            if (val == to_byte(SlipByte::EscEnd)) {
                output.push_back(to_byte(SlipByte::End));
            } else if (val == to_byte(SlipByte::EscEsc)) {
                output.push_back(to_byte(SlipByte::Escape));
            }
            // else: ignore malformed escape
        } else {
            output.push_back(val);
        }
    }

    return output;
}

inline ByteBuffer encodeSLIP(const ByteBuffer& input)
{
    ByteBuffer output;
    output.reserve(input.size() * 2U + 2U);  // worst case, double size + 2 ENDs

    // Avoids a compiler warning with some GCC versions about push_back on an
    // empty-but-reserved vector.
    output.resize(1);
    output[0] = to_byte(SlipByte::End);

    for (std::uint8_t val : input) {
        if (val == to_byte(SlipByte::End) || val == to_byte(SlipByte::Escape)) {
            output.push_back(to_byte(SlipByte::Escape));
            if (val == to_byte(SlipByte::End)) {
                output.push_back(to_byte(SlipByte::EscEnd));
            } else {
                output.push_back(to_byte(SlipByte::EscEsc));
            }
        } else {
            output.push_back(val);
        }
    }

    output.push_back(to_byte(SlipByte::End));
    return output;
}

} // namespace fujinet::io::protocol
