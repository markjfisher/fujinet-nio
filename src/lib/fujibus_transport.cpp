#include "fujinet/io/transport/fujibus_transport.h"

#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <iostream>   // temporary for debug
#include <algorithm>  // for std::find

namespace fujinet::io {

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
        std::cout << "[FujiBusTransport] invalid FujiBus frame, dropped\n";
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

    std::cout << "[FujiBusTransport] receive: id=" << outReq.id
              << " deviceId=0x" << std::hex << static_cast<int>(outReq.deviceId) << std::dec
              << " command=0x" << std::hex << outReq.command << std::dec
              << " params=" << outReq.params.size()
              << " payloadSize=" << outReq.payload.size()
              << std::endl;

    return true;
}

void FujiBusTransport::send(const IOResponse& resp)
{
    std::cout << "[FujiBusTransport] send: deviceId=0x"
              << std::hex << static_cast<int>(resp.deviceId) << std::dec
              << " status=" << static_cast<int>(resp.status)
              << " command=0x" << std::hex << resp.command << std::dec
              << " payloadSize=" << resp.payload.size()
              << std::endl;

    // Map IOResponse → FujiBusPacket.
    //
    // For now we:
    //   - use the same deviceId and command as the request/response pair
    //   - encode status as a 1-byte parameter (first param)
    //   - use IOResponse::payload as the FujiBus "data" block
    //
    // This is easy to evolve later as we tighten to FEP-004 specifics.

    ByteBuffer data;
    data.insert(data.end(), resp.payload.begin(), resp.payload.end());

    WireDeviceId  dev = static_cast<WireDeviceId>(resp.deviceId);
    std::uint8_t cmd = static_cast<std::uint8_t>(resp.command & 0xFF);

    // status as first param, plus payload as raw data.
    FujiBusPacket packet(
        dev,
        cmd,
        static_cast<std::uint8_t>(resp.status), // status param
        std::move(data)
    );

    ByteBuffer serialized = packet.serialize();
    if (!serialized.empty()) {
        _channel.write(serialized.data(), serialized.size());
    }
}

} // namespace fujinet::io
