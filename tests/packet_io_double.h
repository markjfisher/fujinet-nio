#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_io.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"

#include <algorithm>
#include <deque>
#include <utility>

namespace packet_io_test {
using namespace fujinet::io;
using protocol::ByteBuffer;

// Opaque software double, not a FujiBus service or hardware protocol model.
// RX (including scheduled/partial records) and TX each have explicit record and
// byte budgets. Oversize records use length metadata without allocating payload.
class PacketIODouble : public IPacketIO {
public:
    explicit PacketIODouble(std::size_t capacity = 64,
                            std::size_t recordLimit = 4,
                            std::size_t byteLimit = 256)
        : IPacketIO(capacity), _recordLimit(recordLimit), _byteLimit(byteLimit) {}

    bool enqueue(const ByteBuffer& bytes, unsigned delaySteps = 0) {
        return add(bytes, bytes.size(), true, delaySteps);
    }
    bool enqueuePartial(const ByteBuffer& bytes, std::size_t completeSize,
                        unsigned delaySteps = 0) {
        if (bytes.size() >= completeSize || completeSize > capacity()) return false;
        return add(bytes, completeSize, false, delaySteps);
    }
    bool enqueueOversized(std::size_t declaredSize) {
        if (declaredSize <= capacity()) return false;
        return add({}, declaredSize, true, 0);
    }
    bool appendFront(const ByteBuffer& bytes, bool complete) {
        if (_rx.empty() || _rx.front().complete || _rx.front().truncated) return false;
        auto& record = _rx.front();
        if (bytes.size() > _byteLimit - _rxBytes ||
            bytes.size() > record.length - record.bytes.size()) return false;
        if (complete && record.bytes.size() + bytes.size() != record.length) return false;
        record.bytes.insert(record.bytes.end(), bytes.begin(), bytes.end());
        _rxBytes += bytes.size();
        record.complete = complete;
        return true;
    }
    bool truncateFront() {
        if (_rx.empty() || _rx.front().complete) return false;
        _rx.front().truncated = true;
        return true;
    }
    // One explicit step progresses at most recordLimit scheduled records.
    void advance() {
        advanceWork = 0;
        for (auto& record : _rx) {
            ++advanceWork;
            if (record.delaySteps) --record.delaySteps;
        }
    }

    PacketReceiveResult receive(std::uint8_t* buffer, std::size_t limit) override {
        ++receiveCalls;
        if (_resetRequired) return {PacketIOStatus::ResetRequired};
        if (_unknownCompletion) return {PacketIOStatus::UnknownCompletion};
        if (!peerAvailable) return {PacketIOStatus::Unavailable};
        if (_rx.empty() || _rx.front().delaySteps) return {PacketIOStatus::NoData};
        const auto& record = _rx.front();
        if (!record.complete && !record.truncated) return {PacketIOStatus::Incomplete};
        PacketReceiveResult result{PacketIOStatus::Ok, record.length};
        if (record.truncated) result = {PacketIOStatus::Truncated};
        else if (record.length == 0) result = {PacketIOStatus::EmptyPacket};
        else if (record.length > limit || record.length > capacity())
            result = {PacketIOStatus::Oversized};
        else std::copy(record.bytes.begin(), record.bytes.end(), buffer);
        _rxBytes -= record.bytes.size();
        _rx.pop_front();
        return result;
    }

    // Only send outcomes may be injected. Optional local acceptance models
    // ambiguity without claiming any remote execution or effect.
    bool setNextSendResult(PacketIOStatus status, bool acceptBeforeUnknown = false) {
        switch (status) {
        case PacketIOStatus::Ok:
        case PacketIOStatus::Backpressure:
        case PacketIOStatus::Unavailable:
        case PacketIOStatus::SendFailed:
        case PacketIOStatus::UnknownCompletion:
        case PacketIOStatus::ResetRequired:
        case PacketIOStatus::EmptyPacket:
        case PacketIOStatus::Oversized:
            break;
        default:
            return false;
        }
        if (acceptBeforeUnknown && status != PacketIOStatus::UnknownCompletion) return false;
        _nextSendResult = status;
        _acceptBeforeUnknown = acceptBeforeUnknown;
        return true;
    }

    PacketIOStatus send(const std::uint8_t* bytes, std::size_t size) override {
        ++sendCalls;
        if (_resetRequired) return PacketIOStatus::ResetRequired;
        if (_unknownCompletion) return PacketIOStatus::UnknownCompletion;
        if (!peerAvailable) return PacketIOStatus::Unavailable;
        if (size == 0) return PacketIOStatus::EmptyPacket;
        if (size > capacity()) return PacketIOStatus::Oversized;
        const auto fault = _nextSendResult;
        const bool accept = fault == PacketIOStatus::Ok || _acceptBeforeUnknown;
        _nextSendResult = PacketIOStatus::Ok;
        _acceptBeforeUnknown = false;
        if (accept) {
            if (_tx.size() == _recordLimit || size > _byteLimit - _txBytes)
                return PacketIOStatus::Backpressure;
            _tx.emplace_back(bytes, bytes + size);
            _txBytes += size;
            ++acceptedCount;
        }
        if (fault == PacketIOStatus::UnknownCompletion) _unknownCompletion = true;
        if (fault == PacketIOStatus::ResetRequired) _resetRequired = true;
        return fault;
    }

    PacketIOStatus reset() override {
        ++resetCalls;
        if (!resetFailure || !retainOnResetFailure) {
            _rx.clear();
            _tx.clear();
            _rxBytes = _txBytes = 0;
        }
        _nextSendResult = PacketIOStatus::Ok;
        _acceptBeforeUnknown = false;
        _resetRequired = resetFailure;
        return resetFailure ? PacketIOStatus::ResetFailed : PacketIOStatus::Ok;
    }

    bool takeSent(ByteBuffer& out) {
        out.clear();
        if (_resetRequired || _unknownCompletion || _tx.empty()) return false;
        out = std::move(_tx.front());
        _tx.pop_front();
        _txBytes -= out.size();
        return true;
    }
    std::size_t rxRecords() const { return _rx.size(); }
    std::size_t txRecords() const { return _tx.size(); }
    std::size_t rxBytes() const { return _rxBytes; }
    std::size_t txBytes() const { return _txBytes; }

    bool peerAvailable{true};
    bool resetFailure{false};
    bool retainOnResetFailure{false};
    std::size_t acceptedCount{0};
    std::size_t receiveCalls{0}, sendCalls{0}, resetCalls{0}, advanceWork{0};

private:
    PacketIOStatus _nextSendResult{PacketIOStatus::Ok};
    bool _acceptBeforeUnknown{false};
    struct Record {
        ByteBuffer bytes;
        std::size_t length;
        bool complete;
        unsigned delaySteps;
        bool truncated{false};
    };
    bool add(const ByteBuffer& bytes, std::size_t length, bool complete, unsigned delay) {
        if (_rx.size() == _recordLimit || bytes.size() > _byteLimit - _rxBytes ||
            bytes.size() > capacity()) return false;
        _rx.push_back({bytes, length, complete, delay});
        _rxBytes += bytes.size();
        return true;
    }
    const std::size_t _recordLimit, _byteLimit;
    std::deque<Record> _rx;
    std::deque<ByteBuffer> _tx;
    std::size_t _rxBytes{0}, _txBytes{0};
    bool _resetRequired{false}, _unknownCompletion{false};
};

// Byte methods count accidental fallbacks; capability can be changed to test
// that a framer never changes adapter identity or releases a prior adapter slot.
class PacketChannel : public Channel {
public:
    explicit PacketChannel(IPacketIO* adapter = nullptr) : adapter(adapter) {}
    IPacketIO* packet_io() override { return adapter; }
    bool available() override { ++byteCalls; return false; }
    std::size_t read(std::uint8_t*, std::size_t) override { ++byteCalls; return 0; }
    void write(const std::uint8_t*, std::size_t) override { ++byteCalls; }
    IPacketIO* adapter;
    std::size_t byteCalls{0};
};
} // namespace packet_io_test
