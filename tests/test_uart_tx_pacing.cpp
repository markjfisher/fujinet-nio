#include "doctest.h"

#include "fujinet/io/uart_tx_pacing.h"

#include <cstdint>
#include <vector>

using fujinet::io::kUartTxByteGapUsAmiga38400Start;
using fujinet::io::kUartTxChunkGapUsAmiga38400;
using fujinet::io::kUartTxChunkSizeAmiga38400;
using fujinet::io::next_uart_tx_slice;
using fujinet::io::uart_tx_pacing_active;
using fujinet::io::UartTxSlice;

namespace {

std::vector<UartTxSlice> plan(std::size_t len,
                              std::uint32_t byte_gap_us,
                              std::uint32_t chunk_size,
                              std::uint32_t chunk_gap_us)
{
    std::vector<UartTxSlice> out;
    std::size_t offset = 0;
    UartTxSlice slice;
    while (next_uart_tx_slice(len, byte_gap_us, chunk_size, chunk_gap_us, offset, slice)) {
        out.push_back(slice);
    }
    return out;
}

} // namespace

TEST_CASE("uart TX pacing: unpaced write is a single burst")
{
    CHECK_FALSE(uart_tx_pacing_active(0, 0, 0));
    CHECK_FALSE(uart_tx_pacing_active(0, 16, 0));
    CHECK_FALSE(uart_tx_pacing_active(0, 0, 1000));

    const auto slices = plan(512, 0, 0, 0);
    REQUIRE(slices.size() == 1);
    CHECK(slices[0].offset == 0);
    CHECK(slices[0].length == 512);
    CHECK(slices[0].gap_after_us == 0);
}

TEST_CASE("uart TX pacing: 750 µs inter-byte is one byte plus gap except last")
{
    CHECK(kUartTxByteGapUsAmiga38400Start == 750u);
    CHECK(uart_tx_pacing_active(kUartTxByteGapUsAmiga38400Start, 0, 0));

    const auto slices = plan(4, kUartTxByteGapUsAmiga38400Start, 0, 0);
    REQUIRE(slices.size() == 4);
    for (std::size_t i = 0; i < slices.size(); ++i) {
        CHECK(slices[i].offset == i);
        CHECK(slices[i].length == 1);
        CHECK(slices[i].gap_after_us == (i + 1 < slices.size() ? 750u : 0u));
    }
}

TEST_CASE("uart TX pacing: single byte has no trailing gap")
{
    const auto slices = plan(1, 750, 8, 1000);
    REQUIRE(slices.size() == 1);
    CHECK(slices[0].length == 1);
    CHECK(slices[0].gap_after_us == 0);
}

TEST_CASE("uart TX pacing: chunks burst then idle between chunks")
{
    CHECK(uart_tx_pacing_active(0, 8, 1000));

    const auto slices = plan(20, 0, 8, 1000);
    REQUIRE(slices.size() == 3);
    CHECK(slices[0].offset == 0);
    CHECK(slices[0].length == 8);
    CHECK(slices[0].gap_after_us == 1000);
    CHECK(slices[1].offset == 8);
    CHECK(slices[1].length == 8);
    CHECK(slices[1].gap_after_us == 1000);
    CHECK(slices[2].offset == 16);
    CHECK(slices[2].length == 4);
    CHECK(slices[2].gap_after_us == 0);
}

TEST_CASE("uart TX pacing: exact chunk multiple has no trailing gap")
{
    const auto slices = plan(16, 0, 8, 500);
    REQUIRE(slices.size() == 2);
    CHECK(slices[0].gap_after_us == 500);
    CHECK(slices[1].length == 8);
    CHECK(slices[1].gap_after_us == 0);
}

TEST_CASE("uart TX pacing: byte gap takes precedence over chunk pacing")
{
    const auto slices = plan(3, 750, 16, 2000);
    REQUIRE(slices.size() == 3);
    CHECK(slices[0].length == 1);
    CHECK(slices[0].gap_after_us == 750);
    CHECK(slices[2].gap_after_us == 0);
}

TEST_CASE("uart TX pacing: product 16/2000 chunks a 512-byte write")
{
    CHECK(kUartTxChunkSizeAmiga38400 == 16u);
    CHECK(kUartTxChunkGapUsAmiga38400 == 2000u);
    CHECK(uart_tx_pacing_active(0, kUartTxChunkSizeAmiga38400,
                                kUartTxChunkGapUsAmiga38400));

    const auto slices = plan(512, 0, kUartTxChunkSizeAmiga38400,
                             kUartTxChunkGapUsAmiga38400);
    REQUIRE(slices.size() == 32);
    CHECK(slices[0].length == 16);
    CHECK(slices[0].gap_after_us == 2000);
    CHECK(slices[30].gap_after_us == 2000);
    CHECK(slices[31].offset == 496);
    CHECK(slices[31].length == 16);
    CHECK(slices[31].gap_after_us == 0);
}

TEST_CASE("uart TX pacing: empty write yields no slices")
{
    CHECK(plan(0, 750, 8, 1000).empty());
}
