#include "fujinet/io/transport/native_framer.h"

namespace fujinet::io {

void NativeFramer::poll(Channel& ch)
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

bool NativeFramer::nextPacket(ByteBuffer& outPacket)
{
    if (_rxBuffer.empty()) {
        return false;
    }
    // Stub assumption: everything accumulated since the last nextPacket() call
    // is one complete datagram. Valid for packet-native channels (Zorro, SPI)
    // where the channel delivers complete frames per read. If multiple poll()
    // calls occur before nextPacket(), all bytes are merged — fix when a real
    // datagram channel with per-read boundaries is wired.
    outPacket = std::move(_rxBuffer);
    return true;
}

void NativeFramer::sendPacket(Channel& ch, const ByteBuffer& packet)
{
    if (!packet.empty()) {
        ch.write(packet.data(), packet.size());
    }
}

} // namespace fujinet::io
