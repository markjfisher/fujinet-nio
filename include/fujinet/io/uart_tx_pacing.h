#pragma once

#include <cstddef>
#include <cstdint>

namespace fujinet::io {

/// First Amiga RS-232 38,400 inter-byte experiment (character time + gap ≈ 9600).
inline constexpr std::uint32_t kUartTxByteGapUsAmiga38400Start = 750;
/// Product ESP→host pacing at 38,400: 16-byte bursts, 2000 µs between chunks.
inline constexpr std::uint32_t kUartTxChunkSizeAmiga38400 = 16;
inline constexpr std::uint32_t kUartTxChunkGapUsAmiga38400 = 2000;

struct UartTxSlice {
    std::size_t offset{0};
    std::size_t length{0};
    std::uint32_t gap_after_us{0};
};

inline bool uart_tx_pacing_active(std::uint32_t byte_gap_us,
                                  std::uint32_t chunk_size,
                                  std::uint32_t chunk_gap_us) noexcept
{
    return byte_gap_us != 0 || (chunk_size != 0 && chunk_gap_us != 0);
}

/// Next slice of one `Channel::write`. Start `offset` at 0. Returns false when
/// the write is complete. Byte-gap pacing takes precedence over chunk pacing.
/// Unpaced writes yield a single slice covering `len`.
inline bool next_uart_tx_slice(std::size_t len,
                               std::uint32_t byte_gap_us,
                               std::uint32_t chunk_size,
                               std::uint32_t chunk_gap_us,
                               std::size_t& offset,
                               UartTxSlice& slice) noexcept
{
    if (offset >= len) {
        return false;
    }

    const std::size_t remaining = len - offset;
    slice.offset = offset;

    if (byte_gap_us != 0) {
        slice.length = 1;
        slice.gap_after_us = (remaining > 1) ? byte_gap_us : 0;
    } else if (chunk_size != 0 && chunk_gap_us != 0) {
        const std::size_t max_chunk = static_cast<std::size_t>(chunk_size);
        const std::size_t n = (remaining < max_chunk) ? remaining : max_chunk;
        slice.length = n;
        slice.gap_after_us = (remaining > n) ? chunk_gap_us : 0;
    } else {
        slice.length = remaining;
        slice.gap_after_us = 0;
    }

    offset += slice.length;
    return true;
}

} // namespace fujinet::io
