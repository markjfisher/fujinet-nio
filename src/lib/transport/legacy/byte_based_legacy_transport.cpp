#include "fujinet/io/transport/legacy/byte_based_legacy_transport.h"
#include "fujinet/core/logging.h"

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "byte_legacy";

ByteBasedLegacyTransport::ByteBasedLegacyTransport(
    Channel& channel,
    const BusTraits& traits
)
    : LegacyTransport(channel, traits)
{
}

ByteBasedLegacyTransport::~ByteBasedLegacyTransport() = default;

std::size_t ByteBasedLegacyTransport::expectedDataFrameLength(const cmdFrame_t& /*frame*/) const
{
    return 256;
}

bool ByteBasedLegacyTransport::receive(IORequest& outReq) {
    // If we're waiting for data, handle that first
    if (_state == State::WaitingForData) {
        const std::size_t expectedLen = expectedDataFrameLength(_pendingFrame);
        if (expectedLen == 0) {
            sendNak();
            _state = State::WaitingForCommand;
            return false;
        }
        
        // Safety cap: legacy buses encode sizes in small integers; avoid unbounded allocations.
        constexpr std::size_t MAX_DATA_FRAME = 65535;
        if (expectedLen > MAX_DATA_FRAME) {
            FN_LOGW(TAG, "expected data frame too large: %zu", expectedLen);
            sendNak();
            _state = State::WaitingForCommand;
            return false;
        }

        std::vector<std::uint8_t> buffer;
        buffer.resize(expectedLen);
        std::size_t bytesRead = readDataFrame(buffer.data(), buffer.size());
        
        if (bytesRead > 0) {
            // Update the pending request with the data payload
            outReq = convertToIORequest(_pendingFrame);
            outReq.payload.assign(buffer.begin(), buffer.begin() + bytesRead);
            _state = State::WaitingForCommand;
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
    
    return true;
}

void ByteBasedLegacyTransport::send(const IOResponse& resp) {
    FN_LOGI(TAG, "send(): Received response: id=%u, device=0x%02X, command=0x%02X, status=%d, payload_size=%zu",
            resp.id, resp.deviceId, resp.command, static_cast<int>(resp.status), resp.payload.size());

    // Byte-based protocols: ACK/NAK then COMPLETE/ERROR + data
    if (resp.status == StatusCode::Ok) {
        sendComplete();
        if (!resp.payload.empty()) {
            FN_LOGI(TAG, "send(): Writing %zu bytes of payload data", resp.payload.size());
            writeDataFrame(resp.payload.data(), resp.payload.size());
        } else {
            FN_LOGI(TAG, "send(): No payload data to write");
        }
    } else {
        FN_LOGW(TAG, "send(): Response has error status=%d, sending ERROR", static_cast<int>(resp.status));
        sendError();
    }
    
    _state = State::WaitingForCommand;
}

} // namespace fujinet::io::transport::legacy
