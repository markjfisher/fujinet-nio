#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "fujinet/io/core/io_message.h"

namespace fujinet::platform::legacy {

// Response protocol styles for different legacy buses
enum class ResponseStyle : std::uint8_t {
    AckNakThenData,      // SIO: ACK/NAK, then COMPLETE/ERROR + data
    StatusThenData,      // IWM: Status byte, then data packet
    ImmediateData,       // IEC: Data immediately, no status
};

// Checksum function type
using ChecksumFn = std::function<std::uint8_t(const std::uint8_t*, std::size_t)>;

// Device ID mapping function (wire ID → internal DeviceID)
using DeviceIdMapper = std::function<io::DeviceID(std::uint8_t)>;

// Bus-specific traits that vary by platform/protocol
struct BusTraits {
    // Checksum algorithm for command frames
    ChecksumFn checksum;
    
    // Timing constants (microseconds)
    std::uint32_t ack_delay{0};
    std::uint32_t complete_delay{0};
    std::uint32_t error_delay{0};
    
    // Response protocol style
    ResponseStyle response_style{ResponseStyle::AckNakThenData};
    
    // Device ID mapping (wire-level → internal DeviceID)
    DeviceIdMapper map_device_id;
    
    // Validate a command frame checksum
    // frame_data points to the command data (4 bytes: device, comnd, aux1, aux2)
    // checksum is the received checksum byte
    bool validate_checksum(const std::uint8_t* frame_data, std::size_t frame_len, std::uint8_t received_checksum) const {
        std::uint8_t calculated = checksum(frame_data, frame_len);
        return calculated == received_checksum;
    }
};

// Platform-specific factories for bus traits
BusTraits make_sio_traits();

} // namespace fujinet::platform::legacy
