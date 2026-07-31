#pragma once

#include <cstdint>

namespace fujinet::io::protocol {

inline constexpr std::uint8_t APPSTORE_PROTOCOL_VERSION = 1;

enum class AppStoreCommand : std::uint8_t {
    Stat   = 0x01,
    Read   = 0x02,
    Write  = 0x03,
    Delete = 0x04,
    List   = 0x05,
};

inline AppStoreCommand to_app_store_command(std::uint16_t raw)
{
    return static_cast<AppStoreCommand>(static_cast<std::uint8_t>(raw));
}

} // namespace fujinet::io::protocol
