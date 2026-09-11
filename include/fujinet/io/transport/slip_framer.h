#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/io/transport/iframer.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"

namespace fujinet::io {

using protocol::ByteBuffer;

// SlipFramer implements IFramer using SLIP (RFC 1055) framing.
// It has zero FujiBus knowledge — it only deals in raw byte buffers.
class SlipFramer : public IFramer {
public:
    // Drain available bytes from ch into _rxBuffer.
    void poll(Channel& ch) override;

    // Extract and decode one complete SLIP frame into raw opaque packet bytes.
    // Returns true and populates outPacket when a complete frame is ready.
    bool nextPacket(ByteBuffer& outPacket) override;

    // SLIP-escape and delimit one raw opaque packet before writing to ch.
    void sendPacket(Channel& ch, const ByteBuffer& packet) override;

private:
    ByteBuffer _rxBuffer;
};

} // namespace fujinet::io
