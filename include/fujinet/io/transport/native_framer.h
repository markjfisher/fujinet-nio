#pragma once

#include "fujinet/io/transport/iframer.h"
#include "fujinet/io/core/packet_io.h"

namespace fujinet::io {

// One owned opaque receive slot; no byte-channel fallback or protocol parsing.
class NativeFramer : public IFramer {
public:
    NativeFramer() = default;
    explicit NativeFramer(IPacketIO& adapter);
    NativeFramer(const NativeFramer&) = delete;
    NativeFramer& operator=(const NativeFramer&) = delete;
    NativeFramer(NativeFramer&&) = delete;
    NativeFramer& operator=(NativeFramer&&) = delete;

    void poll(Channel& ch) override;
    bool nextPacket(ByteBuffer& outPacket) override;
    void sendPacket(Channel& ch, const ByteBuffer& packet) override;
    PacketIOStatus reset(Channel& ch);

    std::size_t capacity() const { return _storage.size(); }
    PacketIOStatus receiveStatus() const { return _receiveStatus; }
    PacketIOStatus sendStatus() const { return _sendStatus; }
    PacketIOStatus resetStatus() const { return _resetStatus; }
    bool unknownCompletion() const { return _unknownCompletion; }

private:
    void bind(IPacketIO& adapter);
    PacketIOStatus checkAdapter(Channel& ch);

    IPacketIO* _adapter{nullptr}; // Borrowed, stable for this framer's lifetime.
    Channel* _slotChannel{nullptr}; // Must outlive an occupied receive slot.
    ByteBuffer _storage;
    std::size_t _readySize{0};
    bool _resetRequired{false};
    bool _unknownCompletion{false};
    PacketIOStatus _receiveStatus{PacketIOStatus::NoData};
    PacketIOStatus _sendStatus{PacketIOStatus::NoData};
    PacketIOStatus _resetStatus{PacketIOStatus::NoData};
};

} // namespace fujinet::io
