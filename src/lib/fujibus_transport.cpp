#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/fujibus_packet_codec.h"

#include "fujinet/core/logging.h"
#include "fujinet/core/utils.h"

namespace fujinet::io {

static constexpr const char* TAG = "fujibus";

using fujinet::core::log_hexdump;
using fujinet::io::protocol::FujiBusPacket;
using fujinet::io::protocol::ByteBuffer;

void FujiBusTransport::poll()
{
    if (!_lineCoordinator) {
        _framer.poll(_channel);
        return;
    }
    const std::uint64_t before = _framer.bytesTaken();
    _framer.poll(_channel);
    const std::uint64_t taken = _framer.bytesTaken() - before;
    if (taken > 0) {
        _lineCoordinator->bytes_received(static_cast<std::size_t>(taken));
    }
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
//  - The shared packet codec parses the raw bytes into an IORequest.
bool FujiBusTransport::receive(IORequest& outReq)
{
    ByteBuffer frame;
    if (!_framer.nextPacket(frame)) {
        // No packet to deliver. Every request delivered so far has been
        // answered, so this is the idle point where a shared line may be lent.
        if (_lineCoordinator && _lineCoordinator->line_idle(_framer.hasPartial())) {
            _framer.discardPartial();
        }
        return false;
    }

    const bool valid = protocol::decode_fujibus_request(frame, _nextRequestId, outReq);
    if (_lineCoordinator) {
        _lineCoordinator->frame_received(valid);
    }
    if (!valid) {
        FN_LOGW(TAG, "invalid FujiBus frame (response), dropped");
        if (!frame.empty()) {
            FN_LOGW(TAG, "  raw frame (%zu bytes):", frame.size());
            log_hexdump(TAG, frame.data(), frame.size());
        }
        return false;
    }

    ++_nextRequestId;

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

    ByteBuffer serialized = protocol::encode_fujibus_response(resp);
    // Native framers must observe serialization failure too (empty raw output).
    // Byte-stream framers retain their existing empty-packet no-op behavior.
    _framer.sendPacket(_channel, serialized);
}

bool FujiBusTransport::receiveResponse(IOResponse& outResp)
{
    ByteBuffer frame;
    if (!_framer.nextPacket(frame)) {
        return false;
    }

    auto packetPtr = FujiBusPacket::fromRaw(frame);
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
