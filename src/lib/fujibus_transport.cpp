#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include "fujinet/core/logging.h"
#include "fujinet/core/utils.h"

namespace fujinet::io {

static constexpr const char* TAG = "fujibus";

using fujinet::core::log_hexdump;
using fujinet::io::protocol::FujiBusPacket;
using fujinet::io::protocol::ByteBuffer;
using fujinet::io::protocol::WireDeviceId;

void FujiBusTransport::poll()
{
    _framer.poll(_channel);
}

bool FujiBusTransport::supports_work_wait() const
{
    return _channel.supports_readable_wait();
}

bool FujiBusTransport::wait_for_work(std::chrono::milliseconds timeout)
{
    return _channel.wait_for_readable(timeout);
}

// FujiBus framing:
//
//  - poll() delegates to _framer which accumulates raw bytes from the Channel.
//  - receive() asks _framer for the next complete packet.
//  - FujiBusPacket::fromSerialized() parses that into a FujiBusPacket.
//  - We then map FujiBusPacket → IORequest.
bool FujiBusTransport::receive(IORequest& outReq)
{
    ByteBuffer frame;
    if (!_framer.nextPacket(frame)) {
        // No complete frame yet.
        return false;
    }

    auto packetPtr = FujiBusPacket::fromSerialized(frame);
    if (!packetPtr) {
        FN_LOGW(TAG, "invalid FujiBus frame (response), dropped");
        if (!frame.empty()) {
            FN_LOGW(TAG, "  raw frame (%zu bytes):", frame.size());
            log_hexdump(TAG, frame.data(), frame.size());
        }
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

#if defined(FN_DEBUG)
    if (!outReq.params.empty()) {
        FN_LOGI(TAG, "  params:");
        for (std::size_t i = 0; i < outReq.params.size(); ++i) {
            FN_LOGI(TAG, "    [%zu] = 0x%08X", i, (unsigned)outReq.params[i]);
        }
    }
    if (!outReq.payload.empty()) {
        FN_LOGI(TAG, "  payload (%zu bytes):", outReq.payload.size());
        log_hexdump(TAG, outReq.payload.data(), outReq.payload.size());
    }
#endif

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

#if defined(FN_DEBUG)
    if (!resp.payload.empty()) {
        FN_LOGI(TAG, "  payload (%zu bytes):", resp.payload.size());
        log_hexdump(TAG, resp.payload.data(), resp.payload.size());
    }
#endif

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
        _framer.sendPacket(_channel, serialized);
    }
}

bool FujiBusTransport::receiveResponse(IOResponse& outResp)
{
    ByteBuffer frame;
    if (!_framer.nextPacket(frame)) {
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
