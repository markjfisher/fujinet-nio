#include "fujinet/io/transport/slip_framer.h"
#include "fujinet/io/protocol/slip_codec.h"

#include <algorithm>
#include <cstdint>

namespace fujinet::io {

using fujinet::io::protocol::SlipByte;
using fujinet::io::protocol::to_byte;
using fujinet::io::protocol::ByteBuffer;

void SlipFramer::poll(Channel& ch)
{
    std::uint8_t temp[256];
    while (ch.available()) {
        std::size_t n = ch.read(temp, sizeof(temp));
        if (n == 0) {
            break;
        }
        _rxBuffer.insert(_rxBuffer.end(), temp, temp + n);
    }
}

bool SlipFramer::nextPacket(ByteBuffer& outPacket)
{
    outPacket.clear();
    const auto end_marker = to_byte(SlipByte::End);

    // Find the first END marker (discard any leading noise before it).
    auto startIt = std::find(_rxBuffer.begin(), _rxBuffer.end(), end_marker);
    if (startIt == _rxBuffer.end()) {
        _rxBuffer.clear();  // no END in sight — all noise, discard
        return false;
    }

    // Skip any run of consecutive END markers.  RFC 1055 treats them as
    // empty inter-packet separators.  After the loop, startIt points to the
    // last END in the leading run — the actual frame-start delimiter — and
    // contentIt points to the first non-END byte (the payload).
    auto contentIt = std::next(startIt);
    while (contentIt != _rxBuffer.end() && *contentIt == end_marker) {
        startIt = contentIt;
        ++contentIt;
    }

    if (contentIt == _rxBuffer.end()) {
        // Buffer is all END markers — keep just the last one; it may be the
        // start of a frame whose payload has not arrived yet.
        _rxBuffer.erase(_rxBuffer.begin(), startIt);
        return false;
    }

    // Find the terminating END after the payload.
    auto endIt = std::find(contentIt, _rxBuffer.end(), end_marker);
    if (endIt == _rxBuffer.end()) {
        // Incomplete frame — discard noise before startIt and wait.
        _rxBuffer.erase(_rxBuffer.begin(), startIt);
        return false;
    }

    // Complete frame: [startIt (END) ... endIt (END)] inclusive.
    outPacket = protocol::decodeSLIP(ByteBuffer(startIt, std::next(endIt)));
    _rxBuffer.erase(_rxBuffer.begin(), std::next(endIt));
    // Malformed escape-only frames can decode to nothing. Consume but do not
    // deliver them; a later call can extract any following frame.
    return !outPacket.empty();
}

void SlipFramer::sendPacket(Channel& ch, const ByteBuffer& packet)
{
    // Encode the opaque raw packet once. Empty packets are silently dropped.
    if (!packet.empty()) {
        const auto frame = protocol::encodeSLIP(packet);
        ch.write(frame.data(), frame.size());
    }
}

} // namespace fujinet::io
