// Derived from palm-sync (https://github.com/jichu4n/palm-sync),
// Copyright Chuan Ji, Apache License 2.0. See docs/hotsync.md.

#include "fujinet/hotsync/netsync_transport.h"

#include "fujinet/core/logging.h"

#include <iterator>

namespace fujinet::hotsync {

namespace {

constexpr const char* TAG = "hotsync";

constexpr std::size_t kHeaderSize = 6; // type (always 1), xid, u32 length
constexpr std::uint32_t kHandshakeWaitMs = 10000;
// Messages above this are corrupt framing, not a real DLP request.
constexpr std::uint32_t kMaxMessage = 1024 * 1024;

// The handshake payloads are opaque; these are the bytes palm-sync and
// pilot-link send, captured from Palm Desktop. Only the request lengths are
// checked.
constexpr std::size_t kRequest1Size = 22;
constexpr std::uint8_t kResponse1[] = {
    0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x24, 0xff, 0xff, 0xff, 0xff, 0x3c, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8, 0x01, 0x21, 0x04, 0x27, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr std::size_t kRequest2Size = 50;
constexpr std::uint8_t kResponse2[] = {
    0x13, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x20, 0xff, 0xff, 0xff, 0xff, 0x00, 0x3c, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr std::size_t kRequest3Size = 8;

} // namespace

bool NetSyncTransport::accept()
{
    const bool ok = expect(kRequest1Size) &&
                    send(Bytes(std::begin(kResponse1), std::end(kResponse1))) &&
                    expect(kRequest2Size) &&
                    send(Bytes(std::begin(kResponse2), std::end(kResponse2))) &&
                    expect(kRequest3Size);
    if (!ok) {
        FN_LOGW(TAG, "NetSync handshake failed");
    }
    return ok;
}

bool NetSyncTransport::expect(std::size_t length)
{
    Bytes message;
    return receive(message, kHandshakeWaitMs) && message.size() == length;
}

bool NetSyncTransport::send(const Bytes& message)
{
    _xid = static_cast<std::uint8_t>((_xid % 0xFE) + 1);
    Bytes frame{1, _xid};
    put_be32(frame, static_cast<std::uint32_t>(message.size()));
    frame.insert(frame.end(), message.begin(), message.end());
    return _link.write(frame.data(), frame.size()) == static_cast<int>(frame.size());
}

bool NetSyncTransport::receive(Bytes& message, std::uint32_t timeoutMs)
{
    std::uint8_t header[kHeaderSize];
    if (!read_exact(header, sizeof(header), timeoutMs)) {
        return false;
    }
    const std::uint32_t length = get_be32(header + 2);
    if (length > kMaxMessage) {
        return false;
    }
    message.resize(length);
    return read_exact(message.data(), message.size(), timeoutMs);
}

bool NetSyncTransport::read_exact(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs)
{
    std::size_t done = 0;
    while (done < len) {
        const int got = _link.read(buf + done, len - done, timeoutMs);
        if (got <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(got);
    }
    return true;
}

} // namespace fujinet::hotsync
