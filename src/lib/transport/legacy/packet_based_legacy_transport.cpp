#include "fujinet/io/transport/legacy/packet_based_legacy_transport.h"
#include "fujinet/core/logging.h"

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "packet_legacy";

PacketBasedLegacyTransport::PacketBasedLegacyTransport(
    Channel& channel,
    const BusTraits& traits
)
    : LegacyTransport(channel, traits)
{
}

bool PacketBasedLegacyTransport::receive(IORequest& outReq) {
    // Packet-based protocols read command packets (not simple frames)
    cmdFrame_t frame;
    if (!readCommandFrame(frame)) {
        return false; // No complete packet yet
    }
    
    // Packet-based protocols validate checksum during packet decoding
    // If we get here, the packet was valid
    
    // Convert to IORequest
    outReq = convertToIORequest(frame);
    
    // Check if command needs data packet
    if (commandNeedsData(frame.comnd)) {
        _pendingFrame = frame;
        _state = State::WaitingForData;
    } else {
        _state = State::WaitingForCommand;
    }
    
    return true;
}

void PacketBasedLegacyTransport::send(const IOResponse& resp) {
    // Packet-based protocols: status packet, then data packet (if needed)
    std::uint8_t status_byte = 0;
    
    // Map StatusCode to protocol-specific status byte
    switch (resp.status) {
        case StatusCode::Ok:
            status_byte = 0x00; // SP_ERR_NOERROR
            break;
        case StatusCode::InvalidRequest:
            status_byte = 0x01; // SP_ERR_BADCMD
            break;
        case StatusCode::IOError:
            status_byte = 0x27; // SP_ERR_IOERROR
            break;
        case StatusCode::NotReady:
            status_byte = 0x2F; // SP_ERR_OFFLINE
            break;
        default:
            status_byte = 0x01; // SP_ERR_BADCMD (generic error)
            break;
    }
    
    // Send status packet
    sendStatusPacket(status_byte);
    
    // If response has payload, send data packet
    if (!resp.payload.empty()) {
        sendDataPacket(resp.payload.data(), resp.payload.size());
    }
    
    _state = State::WaitingForCommand;
}

} // namespace fujinet::io::transport::legacy
