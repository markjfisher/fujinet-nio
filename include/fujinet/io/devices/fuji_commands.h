#pragma once
#include <cstdint>

namespace fujinet::io::protocol {

enum class FujiCommand : std::uint8_t {
    GetInfo     = 0x01,
    GetHsio     = 0x02,
    SetHsio     = 0x03,
    Reset       = 0xFF,
    GetSsid     = 0xFE,
    // Add FujiDevice-specific commands only
};

inline FujiCommand to_fuji_command(std::uint16_t raw)
{
    return static_cast<FujiCommand>(static_cast<std::uint8_t>(raw));
}

} // namespace fujinet::io::protocol
