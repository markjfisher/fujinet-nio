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

bool ByteBasedLegacyTransport::receive(IORequest& outReq) {
    // If we're waiting for data, handle that first
    if (_state == State::WaitingForData) {
        // Read data frame (platform-specific)
        // Data reading is handled by platform-specific transport
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
    
    // Send ACK (byte-based protocol)
    sendAck();
    
    // Convert to IORequest
    outReq = convertToIORequest(frame);
    
    // Check if command needs data frame
    if (commandNeedsData(frame.comnd)) {
        _pendingFrame = frame;
        _state = State::WaitingForData;
    } else {
        _state = State::WaitingForCommand;
    }
    
    return true;
}

void ByteBasedLegacyTransport::send(const IOResponse& resp) {
    // Byte-based protocols: ACK/NAK then COMPLETE/ERROR + data
    if (resp.status == StatusCode::Ok) {
        sendComplete();
        if (!resp.payload.empty()) {
            writeDataFrame(resp.payload.data(), resp.payload.size());
        }
    } else {
        sendError();
    }
    
    _state = State::WaitingForCommand;
}

} // namespace fujinet::io::transport::legacy
