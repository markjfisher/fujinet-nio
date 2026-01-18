#pragma once

#include <cstdint>
#include <cstring>

namespace fujinet::io::transport::legacy {

// Common command frame structure used by legacy buses
// Platform-specific buses may have slight variations, but this covers the common case
struct cmdFrame_t {
    std::uint8_t device;   // Device ID on the bus
    std::uint8_t comnd;    // Command byte
    std::uint8_t aux1;     // Auxiliary byte 1
    std::uint8_t aux2;     // Auxiliary byte 2
    std::uint8_t checksum; // Frame checksum
    
    // Convenience accessors
    std::uint32_t commanddata() const {
        std::uint32_t result = 0;
        std::memcpy(&result, this, 4);
        return result;
    }
    
    // Get aux bytes as 16-bit value (little-endian)
    std::uint16_t aux12() const {
        return static_cast<std::uint16_t>(aux1) | (static_cast<std::uint16_t>(aux2) << 8);
    }
};

static_assert(sizeof(cmdFrame_t) == 5, "cmdFrame_t must be exactly 5 bytes");

} // namespace fujinet::io::transport::legacy
