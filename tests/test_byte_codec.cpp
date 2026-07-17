#include "doctest.h"

#include "fujinet/io/devices/byte_codec.h"

#include <array>
#include <cstdint>

TEST_CASE("byte codec reads little-endian integers from byte pointers")
{
    const std::array<std::uint8_t, 4> bytes{{0x34, 0x12, 0x78, 0x56}};

    CHECK(fujinet::io::bytecodec::read_u16le(bytes.data()) == 0x1234);
    CHECK(fujinet::io::bytecodec::read_u32le(bytes.data()) == 0x56781234);
}

TEST_CASE("byte codec Reader uses shared little-endian readers")
{
    const std::array<std::uint8_t, 6> bytes{{0xcd, 0xab, 0x78, 0x56, 0x34, 0x12}};
    fujinet::io::bytecodec::Reader reader(bytes.data(), bytes.size());

    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;

    REQUIRE(reader.read_u16le(u16));
    REQUIRE(reader.read_u32le(u32));
    CHECK(u16 == 0xabcd);
    CHECK(u32 == 0x12345678);
    CHECK(reader.remaining() == 0);
}
