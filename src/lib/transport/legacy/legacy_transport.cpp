#include "fujinet/io/transport/legacy/legacy_transport.h"
#include "fujinet/core/logging.h"

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "legacy";

LegacyTransport::LegacyTransport(
    Channel& channel,
    const platform::legacy::BusTraits& traits
)
    : _channel(channel)
    , _traits(traits)
{
}

void LegacyTransport::poll() {
    // Read raw bytes from channel into buffer
    // Platform-specific transports will parse frames in receive()
    std::uint8_t temp[256];
    while (_channel.available()) {
        std::size_t n = _channel.read(temp, sizeof(temp));
        if (n == 0) {
            break;
        }
        _rxBuffer.insert(_rxBuffer.end(), temp, temp + n);
    }
}

bool LegacyTransport::receive(IORequest& outReq) {
    // If we're waiting for data, handle that first
    if (_state == State::WaitingForData) {
        // Read data frame (platform-specific)
        // For now, assume data is read separately
        // This will be handled by platform-specific transport
        _state = State::WaitingForCommand;
        return false; // Data reading handled by platform-specific code
    }
    
    // Try to read a command frame
    cmdFrame_t frame;
    if (!readCommandFrame(frame)) {
        return false; // No complete frame yet
    }
    
    // Validate checksum
    std::uint8_t frame_data[4] = {
        frame.device,
        frame.comnd,
        frame.aux1,
        frame.aux2
    };
    
    if (!_traits.validate_checksum(frame_data, 4, frame.checksum)) {
        FN_LOGW(TAG, "Invalid checksum: calc=0x%02X, recv=0x%02X",
            _traits.checksum(frame_data, 4),
            frame.checksum);
        sendNak();
        return false;
    }
    
    // Send ACK
    sendAck();
    
    // Convert to IORequest
    outReq = convertToIORequest(frame);
    
    // Check if command needs data frame
    if (commandNeedsData(frame.comnd)) {
        _pendingFrame = frame;
        _state = State::WaitingForData;
        // Data will be read in next receive() call or by platform-specific code
    } else {
        _state = State::WaitingForCommand;
    }
    
    return true;
}

void LegacyTransport::send(const IOResponse& resp) {
    switch (_traits.response_style) {
        case platform::legacy::ResponseStyle::AckNakThenData:
            if (resp.status == StatusCode::Ok) {
                sendComplete();
                if (!resp.payload.empty()) {
                    writeDataFrame(resp.payload.data(), resp.payload.size());
                }
            } else {
                sendError();
            }
            break;
            
        case platform::legacy::ResponseStyle::StatusThenData:
            // IWM-style: status packet, then data packet
            // Platform-specific transport will handle this
            FN_LOGW(TAG, "StatusThenData response style not yet implemented");
            break;
            
        case platform::legacy::ResponseStyle::ImmediateData:
            // IEC-style: data immediately
            if (!resp.payload.empty()) {
                writeDataFrame(resp.payload.data(), resp.payload.size());
            }
            break;
    }
    
    _state = State::WaitingForCommand;
}

bool LegacyTransport::commandNeedsData(std::uint8_t command) const {
    // Most SIO commands that write data need a data frame
    // This is a conservative list - platform-specific transports can override
    switch (command) {
        case 'W': // Write sector
        case 'P': // Put sector
        case 'S': // Status (sometimes has data)
            return true;
        default:
            return false;
    }
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
