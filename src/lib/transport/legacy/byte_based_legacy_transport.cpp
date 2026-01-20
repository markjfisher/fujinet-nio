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
        // Don't return true yet - wait for data frame to be read
        // Return false so IOService doesn't process incomplete request
        return false;
    } else {
        _state = State::WaitingForCommand;
    }
    
    // Convert legacy network device IDs to NetworkService before returning
    // This ensures core services never see legacy device IDs
    bool wasConverted = _networkBridge->convertRequest(outReq);
    if (wasConverted) {
        FN_LOGI(TAG, "receive(): Returning converted request: device=0x%02X, command=0x%02X ('%c')",
                outReq.deviceId, outReq.command,
                (outReq.command >= 32 && outReq.command < 127) ? static_cast<char>(outReq.command) : '?');
    } else {
        // If STATUS wasn't converted (no handle), we still need to process it
        // The bridge will handle the response conversion to return "not connected" status
        if (outReq.deviceId >= 0x71 && outReq.deviceId <= 0x78 && outReq.command == 'S') {
            FN_LOGI(TAG, "receive(): Returning STATUS request without conversion (no handle), bridge will handle response");
            // Still return true so it gets processed - convertResponse will create synthetic response
        } else {
            FN_LOGI(TAG, "receive(): Returning non-legacy request: device=0x%02X, command=0x%02X ('%c')",
                    outReq.deviceId, outReq.command,
                    (outReq.command >= 32 && outReq.command < 127) ? static_cast<char>(outReq.command) : '?');
        }
    }
    
    return true;
}

void ByteBasedLegacyTransport::send(const IOResponse& resp) {
    FN_LOGI(TAG, "send(): Received response: id=%u, device=0x%02X, command=0x%02X, status=%d, payload_size=%zu",
            resp.id, resp.deviceId, resp.command, static_cast<int>(resp.status), resp.payload.size());
    
    // Convert NetworkService responses back to legacy format if needed
    // The bridge looks up the original request by response ID
    IOResponse legacyResp = resp;
    bool wasConverted = _networkBridge->convertResponse(legacyResp);
    if (wasConverted) {
        FN_LOGI(TAG, "send(): Response was converted back to legacy format");
    }
    
    // Byte-based protocols: ACK/NAK then COMPLETE/ERROR + data
    if (legacyResp.status == StatusCode::Ok) {
        sendComplete();
        if (!legacyResp.payload.empty()) {
            FN_LOGI(TAG, "send(): Writing %zu bytes of payload data", legacyResp.payload.size());
            writeDataFrame(legacyResp.payload.data(), legacyResp.payload.size());
        } else {
            FN_LOGI(TAG, "send(): No payload data to write");
        }
    } else {
        FN_LOGW(TAG, "send(): Response has error status=%d, sending ERROR", static_cast<int>(legacyResp.status));
        sendError();
    }
    
    _state = State::WaitingForCommand;
}

} // namespace fujinet::io::transport::legacy
