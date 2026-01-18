#include "fujinet/io/transport/legacy/legacy_transport.h"
#include "fujinet/core/logging.h"

namespace fujinet::io::transport::legacy {

LegacyTransport::LegacyTransport(
    Channel& channel,
    const BusTraits& traits
)
    : _channel(channel)
    , _traits(traits)
{
}

void LegacyTransport::poll() {
    // Read raw bytes from channel into buffer
    // Protocol-specific transports will parse frames/packets in receive()
    std::uint8_t temp[256];
    while (_channel.available()) {
        std::size_t n = _channel.read(temp, sizeof(temp));
        if (n == 0) {
            break;
        }
        _rxBuffer.insert(_rxBuffer.end(), temp, temp + n);
    }
}

bool LegacyTransport::commandNeedsData(std::uint8_t command) const {
    // Conservative default - derived classes should override
    // SIO: 'W', 'P', 'S' need data
    // IWM: 0x02, 0x42, 0x04, 0x44, 0x09, 0x49 need data
    return false;
}

IORequest LegacyTransport::convertToIORequest(const cmdFrame_t& frame) {
    IORequest req;
    req.id = _nextRequestId++;
    req.deviceId = _traits.map_device_id(frame.device);
    req.type = RequestType::Command;
    req.command = static_cast<std::uint16_t>(frame.comnd);
    
    // Convert aux1/aux2 to params
    req.params.clear();
    req.params.push_back(static_cast<std::uint32_t>(frame.aux1));
    req.params.push_back(static_cast<std::uint32_t>(frame.aux2));
    
    // Payload will be read separately if needed
    req.payload.clear();
    
    return req;
}

} // namespace fujinet::io::transport::legacy
