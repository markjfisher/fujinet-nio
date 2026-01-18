#include "fujinet/io/transport/legacy/bus_traits.h"
#include "fujinet/io/protocol/wire_device_ids.h"

namespace fujinet::io::transport::legacy {

using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;
using fujinet::io::DeviceID;

// IWM checksum algorithm (XOR-based)
// Note: IWM uses XOR checksum for packet headers and data
// This is a simplified version for command frames
// Full IWM packet encoding/decoding is more complex (groups of 7 bytes)
static std::uint8_t iwm_checksum(const std::uint8_t* buf, std::size_t len) {
    std::uint8_t chk = 0;
    for (std::size_t i = 0; i < len; i++) {
        chk ^= buf[i];
    }
    return chk;
}

// Map IWM wire device IDs to internal DeviceID
// Note: IWM device IDs are assigned dynamically during INIT
// This mapping is used for initial device identification before INIT assigns IDs
static DeviceID map_iwm_device_id(std::uint8_t wire_id) {
    // IWM devices are assigned IDs dynamically during INIT
    // For now, pass through the wire ID
    // The actual device type is determined by the Device Information Block (DIB)
    return wire_id;
}

BusTraits make_iwm_traits() {
    BusTraits traits;
    
    // Checksum algorithm (XOR-based for IWM)
    traits.checksum = iwm_checksum;
    
    // IWM timing constants (microseconds)
    // IWM is phase-based, not time-based like SIO
    // These delays are minimal since IWM uses phase signals
    traits.ack_delay = 0;        // No ACK byte in IWM
    traits.complete_delay = 0;   // No COMPLETE byte in IWM
    traits.error_delay = 0;      // No ERROR byte in IWM
    
    // IWM response style: Status packet, then data packet (if needed)
    traits.response_style = ResponseStyle::StatusThenData;
    
    // Device ID mapping (dynamic assignment during INIT)
    traits.map_device_id = map_iwm_device_id;
    
    return traits;
}

} // namespace fujinet::io::transport::legacy
