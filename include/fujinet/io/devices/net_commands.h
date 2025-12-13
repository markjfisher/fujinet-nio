#pragma once

#include <cstdint>

namespace fujinet::io::protocol {

enum class NetworkCommand : std::uint8_t {
    Open  = 0x01,
    Read  = 0x02,
    Write = 0x03,
    Close = 0x04,
    Info  = 0x05,
};

inline NetworkCommand to_network_command(std::uint16_t raw)
{
    return static_cast<NetworkCommand>(static_cast<std::uint8_t>(raw));
}

} // namespace fujinet::io::protocol


