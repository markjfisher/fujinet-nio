#pragma once

#include <cstdint>

namespace fujinet::io::protocol {

// ModemDevice v1: stream-oriented modem with optional AT interpreter and TCP/Telnet backend.
// Wire device ID: WireDeviceId::ModemService (0xFB).
// See docs/modem_device_protocol.md (to be added).
enum class ModemCommand : std::uint8_t {
    Write   = 0x01,
    Read    = 0x02,
    Status  = 0x03,
    Control = 0x04,
};

inline ModemCommand to_modem_command(std::uint16_t raw)
{
    return static_cast<ModemCommand>(static_cast<std::uint8_t>(raw));
}

} // namespace fujinet::io::protocol


