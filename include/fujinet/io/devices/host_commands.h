#pragma once

#include <cstdint>

namespace fujinet::io {

// HostDevice command IDs (Wire Device ID 0xF0)
//
// Protocol version: 1
//
// All requests use FujiBus framing with device=0xF0.
// Responses include status byte as first parameter.
//
// Command format:
//   Request:  [device][cmd][len_lo][len_hi][checksum][descr][payload...]
//   Response: [device][cmd][len_lo][len_hi][checksum][descr][status][payload...]
//
// Status codes:
//   0x00 = OK
//   0x01 = Invalid slot
//   0x02 = Invalid parameter
//   0x03 = Host not configured
//   0xFF = Unknown command

enum class HostCommand : std::uint8_t {
    // 0x01 - Get all hosts
    // Request:  [version:1]
    // Response: [version:1][host_count:1][host0_type:1][host0_name:32][host0_addr:64]...
    //
    // Returns information about all host slots (up to 8).
    // Each host entry is 97 bytes: type(1) + name(32) + address(64)
    // Total response payload: 2 + (host_count * 97) bytes
    GetHosts = 0x01,

    // 0x02 - Set host configuration
    // Request:  [version:1][slot:1][type:1][name:32][address:64]
    // Response: [version:1][status:1]
    //
    // Configures a host slot. Slot is 0-indexed (0-7).
    // Type: 0=SD, 1=TNFS, 0xFF=disabled/clear
    SetHost = 0x02,

    // 0x03 - Get single host
    // Request:  [version:1][slot:1]
    // Response: [version:1][type:1][name:32][address:64]
    //
    // Returns information about a single host slot.
    // Returns status=0x01 if slot is invalid.
    // Returns status=0x03 if host not configured.
    GetHost = 0x03,
};

// Host type values
enum class HostType : std::uint8_t {
    Disabled = 0xFF,
    SD       = 0x00,
    TNFS     = 0x01,
};

// Host device status codes
enum class HostStatus : std::uint8_t {
    OK              = 0x00,
    InvalidSlot     = 0x01,
    InvalidParam    = 0x02,
    NotConfigured   = 0x03,
    UnknownCommand  = 0xFF,
};

} // namespace fujinet::io
