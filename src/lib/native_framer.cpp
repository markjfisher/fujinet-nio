#include "fujinet/io/transport/native_framer.h"

#include <algorithm>

namespace fujinet::io {

NativeFramer::NativeFramer(IPacketIO& adapter)
{
    bind(adapter);
}

void NativeFramer::bind(IPacketIO& adapter)
{
    _adapter = &adapter;
    _storage.resize(std::min<std::size_t>(adapter.capacity(), 65535));
}

PacketIOStatus NativeFramer::checkAdapter(Channel& ch)
{
    auto* adapter = ch.packet_io();
    if (_adapter && adapter != _adapter) {
        _readySize = 0;
        _resetRequired = true;
        return PacketIOStatus::AdapterChanged;
    }
    if (!adapter) return PacketIOStatus::Unsupported;
    if (!_adapter) bind(*adapter);
    if (_storage.empty()) return PacketIOStatus::InvalidCapacity;
    return PacketIOStatus::Ok;
}

void NativeFramer::poll(Channel& ch)
{
    _receiveStatus = checkAdapter(ch);
    if (_receiveStatus != PacketIOStatus::Ok) return;
    if (_resetRequired) {
        _receiveStatus = PacketIOStatus::ResetRequired;
        return;
    }
    if (_unknownCompletion) {
        _receiveStatus = PacketIOStatus::UnknownCompletion;
        return;
    }
    if (_readySize != 0) {
        _receiveStatus = PacketIOStatus::Backpressure;
        return;
    }
    const auto result = _adapter->receive(_storage.data(), _storage.size());
    _receiveStatus = result.status;
    if (result.status == PacketIOStatus::UnknownCompletion) _unknownCompletion = true;
    if (result.status == PacketIOStatus::ResetRequired) _resetRequired = true;
    if (result.status == PacketIOStatus::Ok) {
        if (result.size == 0) _receiveStatus = PacketIOStatus::EmptyPacket;
        else if (result.size > _storage.size()) _receiveStatus = PacketIOStatus::Oversized;
        else {
            _readySize = result.size;
            _slotChannel = &ch;
        }
    }
}

bool NativeFramer::nextPacket(ByteBuffer& outPacket)
{
    outPacket.clear();
    if (_readySize != 0 && _slotChannel) {
        const auto status = checkAdapter(*_slotChannel);
        if (status != PacketIOStatus::Ok) {
            _receiveStatus = status;
            return false;
        }
    }
    if (_readySize == 0 || _resetRequired || _unknownCompletion) return false;
    outPacket.assign(_storage.begin(), _storage.begin() + _readySize);
    _readySize = 0;
    return true;
}

void NativeFramer::sendPacket(Channel& ch, const ByteBuffer& packet)
{
    _sendStatus = checkAdapter(ch);
    if (_sendStatus != PacketIOStatus::Ok) return;
    if (_resetRequired) {
        _sendStatus = PacketIOStatus::ResetRequired;
        return;
    }
    if (_unknownCompletion) {
        _sendStatus = PacketIOStatus::UnknownCompletion;
        return;
    }
    if (packet.empty()) {
        _sendStatus = PacketIOStatus::EmptyPacket;
        return;
    }
    if (packet.size() > _storage.size()) {
        _sendStatus = PacketIOStatus::Oversized;
        return;
    }
    _sendStatus = _adapter->send(packet.data(), packet.size());
    if (_sendStatus == PacketIOStatus::UnknownCompletion) {
        _unknownCompletion = true;
        _readySize = 0;
    }
    if (_sendStatus == PacketIOStatus::ResetRequired) {
        _resetRequired = true;
        _readySize = 0;
    }
}

PacketIOStatus NativeFramer::reset(Channel& ch)
{
    _readySize = 0;
    _resetRequired = true;
    _resetStatus = checkAdapter(ch);
    if (_resetStatus != PacketIOStatus::Ok) return _resetStatus;
    _resetStatus = _adapter->reset();
    if (_resetStatus == PacketIOStatus::UnknownCompletion) _unknownCompletion = true;
    _resetRequired = _resetStatus != PacketIOStatus::Ok;
    return _resetStatus;
}

} // namespace fujinet::io
