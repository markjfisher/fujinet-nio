#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include "fujinet/core/logging.h"

#include <algorithm>  // for std::find

namespace fujinet::io {

static constexpr const char* TAG = "fujibus";

using fujinet::io::protocol::FujiBusPacket;
using fujinet::io::protocol::ByteBuffer;
using fujinet::io::protocol::SlipByte;
using fujinet::io::protocol::WireDeviceId;

void FujiBusTransport::poll()
{
    std::uint8_t temp[256];

    while (_channel.available()) {
        std::size_t n = _channel.read(temp, sizeof(temp));
        if (n == 0) {
            break;
        }
        _rxBuffer.insert(_rxBuffer.end(), temp, temp + n);
    }

    // All framing is handled in receive() via SLIP + FujiBusPacket.
}

// Helper: try to extract a single SLIP-framed message from _rxBuffer.
// We look for: SlipByte::End ... SlipByte::End, and return that span (inclusive).
static bool extractSlipFrame(std::vector<std::uint8_t>& buffer, ByteBuffer& outFrame)
{
    // Find the first END marker.
    auto startIt = std::find(
        buffer.begin(),
        buffer.end(),
        to_byte(SlipByte::End)
    );
    if (startIt == buffer.end()) {
        // No START/END marker yet.
        return false;
    }

    // Find the next END marker after start.
    auto endIt = std::find(
        std::next(startIt),
        buffer.end(),
        to_byte(SlipByte::End)
    );
    if (endIt == buffer.end()) {
        // We have a start but no complete frame yet.
        // Optionally: discard noise before startIt.
        if (startIt != buffer.begin()) {
            buffer.erase(buffer.begin(), startIt);
        }
        return false;
    }

    // We have a complete frame: [startIt, endIt].
    outFrame.clear();
    outFrame.insert(outFrame.end(), startIt, std::next(endIt));

    // Erase everything up to and including endIt from the buffer.
    buffer.erase(buffer.begin(), std::next(endIt));
    return true;
}

// SLIP + FujiBus framing:
//
//  - poll() accumulates raw bytes from the Channel into _rxBuffer.
//  - receive() looks for one full SLIP frame (END ... END).
//  - FujiBusPacket::fromSerialized() parses that into a FujiBusPacket.
//  - We then map FujiBusPacket → IORequest.
bool FujiBusTransport::receive(IORequest& outReq)
{
    ByteBuffer frame;
    if (!extractSlipFrame(_rxBuffer, frame)) {
        // No complete SLIP frame yet.
        return false;
    }

    auto packetPtr = FujiBusPacket::fromSerialized(frame);
    if (!packetPtr) {
        FN_LOGW(TAG, "invalid FujiBus frame (response), dropped");
        return false;
    }

    const FujiBusPacket& packet = *packetPtr;

    // Map FujiBusPacket → IORequest.
    outReq.id        = _nextRequestId++;
    outReq.deviceId  = static_cast<DeviceID>(packet.device());
    outReq.type      = RequestType::Command; // FujiBus operations are all "commands" at this level.
    outReq.command   = static_cast<std::uint16_t>(packet.command());

    outReq.params.clear();
    outReq.params.reserve(packet.paramCount());
    for (unsigned int i = 0; i < packet.paramCount(); ++i) {
        outReq.params.push_back(packet.param(i));
    }

    outReq.payload.clear();
    if (const auto& dataOpt = packet.data()) {
        outReq.payload.insert(outReq.payload.end(), dataOpt->begin(), dataOpt->end());
    }

    // TODO: change to LOGD to reduce noise after initial debugging
    FN_LOGI(TAG,
        "receive: id=%u dev=0x%02X cmd=0x%02X params=%u payload=%u",
        (unsigned)outReq.id,
        (unsigned)outReq.deviceId,
        (unsigned)(outReq.command & 0xFF),
        (unsigned)outReq.params.size(),
        (unsigned)outReq.payload.size());

    return true;
}

void FujiBusTransport::send(const IOResponse& resp)
{
    FN_LOGI(TAG,
        "send: dev=0x%02X status=%u cmd=0x%02X payload=%u",
        (unsigned)resp.deviceId,
        (unsigned)resp.status,
        (unsigned)(resp.command & 0xFF),
        (unsigned)resp.payload.size());

    // Payload is the device-specific protocol blob.
    ByteBuffer data(resp.payload.begin(), resp.payload.end());

    // FujiBus uses an 8-bit command on-wire.
    const auto dev = static_cast<WireDeviceId>(resp.deviceId);
    const auto cmd = static_cast<std::uint8_t>(resp.command & 0xFF);

    // Convention (transport-local):
    //  - param[0] = status (u8)
    //  - data     = device payload (opaque to transport)
    FujiBusPacket packet(dev, cmd);
    packet.addParamU8(static_cast<std::uint8_t>(resp.status))
          .setData(std::move(data));

    ByteBuffer serialized = packet.serialize();
    if (!serialized.empty()) {
        _channel.write(serialized.data(), serialized.size());
    }
}

bool FujiBusTransport::receiveResponse(IOResponse& outResp)
{
    ByteBuffer frame;
    if (!extractSlipFrame(_rxBuffer, frame)) {
        return false;
    }

    auto packetPtr = FujiBusPacket::fromSerialized(frame);
    if (!packetPtr) {
        FN_LOGW(TAG, "invalid FujiBus frame (response), dropped");
        return false;
    }

    const FujiBusPacket& packet = *packetPtr;

    outResp.id       = _nextRequestId++; // still synthetic (no on-wire correlation yet)
    outResp.deviceId = static_cast<DeviceID>(packet.device());
    outResp.command  = static_cast<std::uint16_t>(packet.command());

    // Convention: param[0] is status (u8)
    std::uint8_t st = static_cast<std::uint8_t>(StatusCode::InternalError);
    if (!packet.tryParamU8(0, st)) {
        FN_LOGW(TAG, "response missing status param[0] u8; defaulting InternalError");
        outResp.status = StatusCode::InternalError;
    } else {
        outResp.status = static_cast<StatusCode>(st);
    }

    outResp.payload.clear();
    if (const auto& dataOpt = packet.data()) {
        outResp.payload.insert(outResp.payload.end(), dataOpt->begin(), dataOpt->end());
    }

    FN_LOGI(TAG,
        "receiveResponse: id=%u dev=0x%02X cmd=0x%02X status=%u payload=%u",
        (unsigned)outResp.id,
        (unsigned)outResp.deviceId,
        (unsigned)(outResp.command & 0xFF),
        (unsigned)outResp.status,
        (unsigned)outResp.payload.size());

    return true;
}


} // namespace fujinet::io
