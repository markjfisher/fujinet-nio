#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"

namespace fujinet::io {

using protocol::ByteBuffer;

// Sits between Channel (raw bytes) and the FujiBus parser.
// A framer accumulates bytes from a channel, yields complete packet payloads,
// and writes framed packets back to the channel.
class IFramer {
public:
    virtual ~IFramer() = default;

    // Drain available bytes from ch into internal state. Called every poll cycle.
    virtual void poll(Channel& ch) = 0;

    // Extract one complete packet payload (without framing bytes).
    // Returns true and populates outPacket when a packet is ready.
    virtual bool nextPacket(ByteBuffer& outPacket) = 0;

    // Frame one packet payload and write it to ch.
    virtual void sendPacket(Channel& ch, const ByteBuffer& packet) = 0;
};

} // namespace fujinet::io
