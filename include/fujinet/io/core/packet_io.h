#pragma once

#include <cstddef>
#include <cstdint>

namespace fujinet::io {

// Operation-specific meanings and obligations: docs/native-packet-contract.md.
enum class PacketIOStatus {
    Ok,
    NoData,
    Incomplete,
    Truncated,
    EmptyPacket,
    Oversized,
    Backpressure,
    Unavailable,
    SendFailed,
    UnknownCompletion,
    ResetFailed,
    ResetRequired,
    Unsupported,
    InvalidCapacity,
    AdapterChanged,
};

struct PacketReceiveResult {
    PacketIOStatus status;
    std::size_t size{0}; // Nonzero only for Ok: exactly one complete opaque packet.
};

// Optional nonblocking packet source/sink. Owns bounded queues and partial
// assembly. Buffers are borrowed only for the duration of a call.
class IPacketIO {
public:
    explicit IPacketIO(std::size_t capacity) : _capacity(capacity) {}
    virtual ~IPacketIO() = default;
    std::size_t capacity() const { return _capacity; }

    // At most one whole record; never expose a prefix. Empty, truncated and
    // oversized records are discarded and reported, even for a smaller buffer.
    virtual PacketReceiveResult receive(std::uint8_t* buffer, std::size_t capacity) = 0;

    // One attempt. Ok means local acceptance, not remote execution. Rejection
    // accepts nothing. UnknownCompletion must block further sends, even after
    // local reset; this interface does not authorize recovery or replay.
    virtual PacketIOStatus send(const std::uint8_t* packet, std::size_t size) = 0;

    // Discard local RX/TX/partial/scheduled data. Failure requires a successful
    // reset before further I/O. Success does not establish remote quiescence.
    virtual PacketIOStatus reset() = 0;

private:
    const std::size_t _capacity;
};

} // namespace fujinet::io
