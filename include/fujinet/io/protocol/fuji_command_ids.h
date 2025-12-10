// include/fujinet/io/protocol/fuji_device_ids.h
#pragma once
#include <cstdint>

namespace fujinet::io::protocol {

enum class FujiCommandId : std::uint8_t {
    Reset             = 0xFF,  // to FujiNet control device
    SpecialQuery      = 0xFF,  // to Network device
    GetSsid           = 0xFE,
    DEBUG             = 0xF0,
    // ...
};

inline FujiCommandId to_fuji_command(std::uint16_t raw)
{
    return static_cast<FujiCommandId>(static_cast<std::uint8_t>(raw));
}


} // namespace fujinet::io::protocol
