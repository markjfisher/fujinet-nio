#pragma once

// Palm protocols are big-endian; these helpers keep the byte order explicit
// instead of relying on packed structs.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::hotsync {

using Bytes = std::vector<std::uint8_t>;

inline void put_be16(Bytes& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

inline void put_be32(Bytes& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

inline std::uint16_t get_be16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t get_be32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

} // namespace fujinet::hotsync
