#pragma once

#include "fujinet/io/transport/iframer.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"

namespace fujinet::io {

using protocol::ByteBuffer;

// Pass-through framer for packet-native channels (Zorro, SPI, floppy/Pico).
// On such channels the physical layer delivers complete datagrams — there is
// no byte-level framing to extract.  poll() accumulates all available bytes
// into a single buffer; nextPacket() returns that buffer as one packet.
class NativeFramer : public IFramer {
public:
    // Drain available bytes from ch into _rxBuffer.
    void poll(Channel& ch) override;

    // Return the entire accumulated buffer as one packet and clear it.
    // Returns false if no bytes were received since the last call.
    bool nextPacket(ByteBuffer& outPacket) override;

    // Write packet bytes verbatim to ch (no framing added).
    void sendPacket(Channel& ch, const ByteBuffer& packet) override;

private:
    ByteBuffer _rxBuffer;
};

} // namespace fujinet::io
