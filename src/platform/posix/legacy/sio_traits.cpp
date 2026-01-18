#include "fujinet/platform/legacy/bus_traits.h"
#include "fujinet/io/protocol/wire_device_ids.h"

namespace fujinet::platform::legacy {

using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;
using fujinet::io::DeviceID;

// SIO checksum algorithm (additive wrap-around)
static std::uint8_t sio_checksum(const std::uint8_t* buf, std::size_t len) {
    unsigned int chk = 0;
    for (std::size_t i = 0; i < len; i++) {
        chk = ((chk + buf[i]) >> 8) + ((chk + buf[i]) & 0xff);
    }
    return static_cast<std::uint8_t>(chk);
}

// Map SIO wire device IDs to internal DeviceID
static DeviceID map_sio_device_id(std::uint8_t wire_id) {
    // FujiNet device
    if (wire_id == 0x70) {
        return to_device_id(WireDeviceId::FujiNet);
    }
    
    // Disk devices (0x31-0x3F)
    if (wire_id >= 0x31 && wire_id <= 0x3F) {
        return wire_id; // Pass through
    }
    
    // Network devices (0x71-0x78)
    if (wire_id >= 0x71 && wire_id <= 0x78) {
        return wire_id; // Pass through
    }
    
    // Printer devices (0x40-0x43)
    if (wire_id >= 0x40 && wire_id <= 0x43) {
        return wire_id; // Pass through
    }
    
    // Clock device
    if (wire_id == 0x45) {
        return to_device_id(WireDeviceId::Clock);
    }
    
    // MIDI device
    if (wire_id == 0x99) {
        return to_device_id(WireDeviceId::Midi);
    }
    
    // Type 3 poll (broadcast)
    if (wire_id == 0x7F) {
        return wire_id; // Pass through for now
    }
    
    // Unknown device - pass through
    return wire_id;
}

BusTraits make_sio_traits() {
    BusTraits traits;
    
    // Checksum algorithm
    traits.checksum = sio_checksum;
    
    // SIO timing constants (microseconds)
    // For POSIX/NetSIO, timing is less critical but we still honor the protocol
    traits.ack_delay = 0;
    traits.complete_delay = 250;
    traits.error_delay = 250;
    
    // SIO response style: ACK/NAK, then COMPLETE/ERROR + data
    traits.response_style = ResponseStyle::AckNakThenData;
    
    // Device ID mapping
    traits.map_device_id = map_sio_device_id;
    
    return traits;
}

} // namespace fujinet::platform::legacy
