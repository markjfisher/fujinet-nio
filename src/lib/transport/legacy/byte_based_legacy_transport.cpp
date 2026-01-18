#include "fujinet/io/transport/legacy/byte_based_legacy_transport.h"
#include "fujinet/io/transport/legacy/legacy_network_bridge.h"
#include "fujinet/core/logging.h"

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "byte_legacy";

ByteBasedLegacyTransport::ByteBasedLegacyTransport(
    Channel& channel,
    const BusTraits& traits
)
    : LegacyTransport(channel, traits)
    , _networkBridge(std::make_unique<LegacyNetworkBridge>())
{
}

ByteBasedLegacyTransport::~ByteBasedLegacyTransport() = default;

bool ByteBasedLegacyTransport::receive(IORequest& outReq) {
    // If we're waiting for data, handle that first
    if (_state == State::WaitingForData) {
        // Read data frame (platform-specific)
        // For SIO, aux1/aux2 may contain length info, but typically we read until checksum
        // For now, read up to a reasonable maximum (256 bytes should cover most cases)
        constexpr std::size_t MAX_DATA_FRAME = 256;
        std::uint8_t buffer[MAX_DATA_FRAME];
        std::size_t bytesRead = readDataFrame(buffer, MAX_DATA_FRAME);
        
        if (bytesRead > 0) {
            // Update the pending request with the data payload
            outReq = convertToIORequest(_pendingFrame);
            outReq.payload.assign(buffer, buffer + bytesRead);
            _state = State::WaitingForCommand;
            
            // Convert legacy network device IDs to NetworkService before returning
            // This ensures core services never see legacy device IDs
            _networkBridge->convertRequest(outReq);
            
            return true;
        }
        
        // No data available yet
        return false;
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
    
    // Send ACK (byte-based protocol)
    sendAck();
    
    // Convert to IORequest
    outReq = convertToIORequest(frame);
    
    // Check if command needs data frame
    if (commandNeedsData(frame.comnd)) {
        _pendingFrame = frame;
        _state = State::WaitingForData;
        // Don't convert legacy network yet - wait for data frame
        return true;
    } else {
        _state = State::WaitingForCommand;
    }
    
    // Convert legacy network device IDs to NetworkService before returning
    // This ensures core services never see legacy device IDs
    _networkBridge->convertRequest(outReq);
    
    return true;
}

void ByteBasedLegacyTransport::send(const IOResponse& resp) {
    // Convert NetworkService responses back to legacy format if needed
    // The bridge looks up the original request by response ID
    IOResponse legacyResp = resp;
    _networkBridge->convertResponse(legacyResp);
    
    // Byte-based protocols: ACK/NAK then COMPLETE/ERROR + data
    if (legacyResp.status == StatusCode::Ok) {
        sendComplete();
        if (!legacyResp.payload.empty()) {
            writeDataFrame(legacyResp.payload.data(), legacyResp.payload.size());
        }
    } else {
        sendError();
    }
    
    _state = State::WaitingForCommand;
}

} // namespace fujinet::io::transport::legacy
