#pragma once

#include <cstdint>

namespace fujinet::io::protocol {

enum class WifiCommand : std::uint8_t {
    GetStatus = 0x01,
    GetConfig = 0x02,
    SetConfig = 0x03,
    Scan = 0x04,
};

inline WifiCommand to_wifi_command(std::uint16_t command)
{
    return static_cast<WifiCommand>(command & 0xffU);
}

} // namespace fujinet::io::protocol
