// Derived from palm-sync (https://github.com/jichu4n/palm-sync),
// Copyright Chuan Ji, Apache License 2.0. See docs/hotsync.md.

// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/padp_transport.h"

#include "fujinet/core/logging.h"

#include <algorithm>
#include <chrono>

namespace fujinet::hotsync {

namespace {

constexpr const char* TAG = "hotsync";

// PADP fragment types. TICKLE keeps a link alive and ABORT ends it.
constexpr std::uint8_t kPadpData = 0x01;
constexpr std::uint8_t kPadpAck = 0x02;

constexpr std::uint8_t kFlagFirst = 0x80;
constexpr std::uint8_t kFlagLast = 0x40;
constexpr std::uint8_t kFlagLongForm = 0x10;

// Palm OS allows 1024-byte fragments, but devices drop the link when a long
// fragment delays our ACK of their traffic, so palm-sync sends half that.
constexpr std::size_t kFragmentSize = 512;
constexpr std::uint32_t kAckWaitMs = 2000;
constexpr int kMaxRetries = 10;

// CMP: the Palm sends WAKEUP at 9600 baud; we answer INIT on xid 0xFF.
constexpr std::uint8_t kCmpWakeup = 1;
constexpr std::uint8_t kCmpInit = 2;
constexpr std::uint8_t kCmpChangeBaud = 0x80;
constexpr std::uint8_t kCmpLongFormPadp = 0x10;
constexpr std::uint8_t kCmpXid = 0xFF;
constexpr std::size_t kCmpPacketSize = 10;

} // namespace

PadpTransport::PadpTransport(Link& link, std::uint32_t maxBaud)
    : _link(link), _reader(link), _maxBaud(maxBaud)
{
}

bool PadpTransport::accept()
{
    return wait_for_wakeup() && answer_wakeup();
}

// A hard limit, not an idle timeout: a line that never goes quiet (a Palm app
// retrying FujiBus requests, or bus bytes at the wrong rate) must not keep a
// cradle window open.
bool PadpTransport::wait_for_wakeup(std::uint32_t timeoutMs)
{
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (_received.empty()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        SlpDatagram datagram;
        if (left <= 0 || !_reader.read_within(datagram, static_cast<std::uint32_t>(left))) {
            return false;
        }
        bool acked = false;
        handle_incoming(datagram, 0, 0, acked);
    }
    const Bytes message = std::move(_received.front());
    _received.pop_front();
    if (message.size() < kCmpPacketSize) {
        return false;
    }
    if (message[0] != kCmpWakeup) {
        FN_LOGW(TAG, "expected CMP WAKEUP, got type %u", message[0]);
        return false;
    }
    _wakeupBaud = get_be32(message.data() + 6);
    return true;
}

bool PadpTransport::answer_wakeup()
{
    const std::uint32_t baud = std::min(_wakeupBaud, _maxBaud);
    const bool changeBaud = baud != kCmpInitialBaud;

    Fragment fragment;
    fragment.type = kPadpData;
    fragment.flags = kFlagFirst | kFlagLast;
    fragment.sizeOrOffset = kCmpPacketSize;
    fragment.data = {kCmpInit,
                     static_cast<std::uint8_t>(kCmpLongFormPadp | (changeBaud ? kCmpChangeBaud : 0)),
                     1, 1, 0, 0};
    put_be32(fragment.data, changeBaud ? baud : 0);
    if (!send_fragment(kCmpXid, fragment)) {
        return false;
    }

    // Any WAKEUP retries that crossed our INIT are stale now.
    _received.clear();
    if (changeBaud) {
        _link.set_baud_rate(baud);
    }
    _baud = baud;
    FN_LOGI(TAG, "CMP handshake complete at %lu baud", static_cast<unsigned long>(baud));
    return true;
}

bool PadpTransport::send(const Bytes& message)
{
    const std::uint8_t xid = next_xid();
    std::size_t offset = 0;
    do {
        const std::size_t piece = std::min(kFragmentSize, message.size() - offset);
        Fragment fragment;
        fragment.type = kPadpData;
        fragment.flags = static_cast<std::uint8_t>((offset == 0 ? kFlagFirst : 0) |
                                                   (offset + piece == message.size() ? kFlagLast : 0));
        if (message.size() > 0xFFFF) {
            fragment.flags |= kFlagLongForm;
        }
        fragment.sizeOrOffset = static_cast<std::uint32_t>(offset == 0 ? message.size() : offset);
        fragment.data.assign(message.begin() + static_cast<std::ptrdiff_t>(offset),
                             message.begin() + static_cast<std::ptrdiff_t>(offset + piece));
        if (!send_fragment(xid, fragment)) {
            return false;
        }
        offset += piece;
    } while (offset < message.size());
    return true;
}

bool PadpTransport::receive(Bytes& message, std::uint32_t timeoutMs)
{
    while (_received.empty()) {
        SlpDatagram datagram;
        if (!_reader.read(datagram, timeoutMs)) {
            return false;
        }
        bool acked = false;
        handle_incoming(datagram, 0, 0, acked);
    }
    message = std::move(_received.front());
    _received.pop_front();
    return true;
}

bool PadpTransport::send_fragment(std::uint8_t xid, const Fragment& fragment)
{
    SlpDatagram datagram;
    datagram.xid = xid;
    datagram.payload = encode_fragment(fragment);
    const Bytes frame = slp_encode(datagram);

    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        if (_link.write(frame.data(), frame.size()) != static_cast<int>(frame.size())) {
            return false;
        }
        if (wait_for_ack(xid, fragment.sizeOrOffset)) {
            return true;
        }
        if (_reader.link_closed()) {
            return false;
        }
        FN_LOGD(TAG, "no ACK for xid %u, retry %d", xid, attempt + 1);
    }
    return false;
}

bool PadpTransport::wait_for_ack(std::uint8_t xid, std::uint32_t sizeOrOffset)
{
    bool acked = false;
    while (!acked) {
        SlpDatagram datagram;
        if (!_reader.read(datagram, kAckWaitMs)) {
            return false;
        }
        handle_incoming(datagram, xid, sizeOrOffset, acked);
    }
    return true;
}

// awaitedXid 0 means no send is waiting for an ACK; xid 0 is never sent.
bool PadpTransport::handle_incoming(const SlpDatagram& datagram, std::uint8_t awaitedXid,
                                    std::uint32_t awaitedOffset, bool& acked)
{
    if (datagram.type != SlpType::Padp) {
        return true;
    }

    Fragment fragment;
    if (!decode_fragment(datagram.payload, fragment)) {
        return false;
    }

    switch (fragment.type) {
    case kPadpAck:
        if (awaitedXid != 0 && datagram.xid == awaitedXid && fragment.sizeOrOffset == awaitedOffset) {
            acked = true;
        }
        return true;
    case kPadpData:
        // A duplicate (our ACK was lost) is never an ACK of what we sent.
        if (is_duplicate(datagram)) {
            send_ack(datagram, fragment);
            return true;
        }
        // A new reply carrying our xid means the ACK for our request was lost.
        if (awaitedXid != 0 && datagram.xid == awaitedXid) {
            acked = true;
        }
        return accept_data(datagram, fragment);
    default:
        // A TICKLE needs no answer; an ABORT surfaces as a timeout upstream.
        return true;
    }
}

// The device resends a fragment verbatim when our ACK was lost.
bool PadpTransport::is_duplicate(const SlpDatagram& datagram) const
{
    return datagram.xid == _lastAckedXid && datagram.payload == _lastAckedPayload;
}

bool PadpTransport::accept_data(const SlpDatagram& datagram, const Fragment& fragment)
{
    if (fragment.flags & kFlagFirst) {
        _assembling.clear();
        _assemblingSize = fragment.sizeOrOffset;
        _assemblingActive = true;
    } else if (!_assemblingActive || fragment.sizeOrOffset != _assembling.size()) {
        FN_LOGW(TAG, "out-of-sequence PADP fragment at %lu",
                static_cast<unsigned long>(fragment.sizeOrOffset));
        _assemblingActive = false;
        return false;
    }

    _assembling.insert(_assembling.end(), fragment.data.begin(), fragment.data.end());
    send_ack(datagram, fragment);
    _lastAckedXid = datagram.xid;
    _lastAckedPayload = datagram.payload;

    if (_assembling.size() >= _assemblingSize) {
        _assemblingActive = false;
        _assembling.resize(_assemblingSize);
        _received.push_back(std::move(_assembling));
        _assembling = Bytes();
    }
    return true;
}

void PadpTransport::send_ack(const SlpDatagram& datagram, const Fragment& fragment)
{
    Fragment ack;
    ack.type = kPadpAck;
    ack.flags = static_cast<std::uint8_t>(kFlagFirst | kFlagLast | (fragment.flags & kFlagLongForm));
    ack.sizeOrOffset = fragment.sizeOrOffset;

    SlpDatagram reply;
    reply.destSocket = datagram.srcSocket;
    reply.srcSocket = datagram.destSocket;
    reply.xid = datagram.xid;
    reply.payload = encode_fragment(ack);
    const Bytes frame = slp_encode(reply);
    _link.write(frame.data(), frame.size());
}

Bytes PadpTransport::encode_fragment(const Fragment& fragment)
{
    Bytes out{fragment.type, fragment.flags};
    if (fragment.flags & kFlagLongForm) {
        put_be32(out, fragment.sizeOrOffset);
    } else {
        put_be16(out, static_cast<std::uint16_t>(fragment.sizeOrOffset));
    }
    out.insert(out.end(), fragment.data.begin(), fragment.data.end());
    return out;
}

bool PadpTransport::decode_fragment(const Bytes& payload, Fragment& out)
{
    if (payload.size() < 4) {
        return false;
    }
    out.type = payload[0];
    out.flags = payload[1];
    std::size_t headerSize = 4;
    if (out.flags & kFlagLongForm) {
        if (payload.size() < 6) {
            return false;
        }
        out.sizeOrOffset = get_be32(payload.data() + 2);
        headerSize = 6;
    } else {
        out.sizeOrOffset = get_be16(payload.data() + 2);
    }
    out.data.assign(payload.begin() + static_cast<std::ptrdiff_t>(headerSize), payload.end());
    return true;
}

std::uint8_t PadpTransport::next_xid()
{
    const std::uint8_t xid = _nextXid;
    _nextXid = static_cast<std::uint8_t>((_nextXid % 0xFE) + 1);
    return xid;
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
