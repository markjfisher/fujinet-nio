#pragma once

#include "fujinet/io/protocol/fuji_bus_packet.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace fujinet::io::visor_usb {
using protocol::ByteBuffer;
constexpr std::size_t packet_capacity = 1024;
struct LogicalPort { unsigned function, port; };
struct Endpoint {
    unsigned configuration, interface, alternate, interfaceClass;
    unsigned address, type, mps;
};
struct EndpointPair { Endpoint input, output; };
// Tested Handspring old connection-information contract; not a universal Palm protocol.
std::optional<std::vector<LogicalPort>> connection_info(std::span<const std::uint8_t> bytes);
std::optional<EndpointPair> select_generic_pair(std::span<const LogicalPort> ports,
    std::span<const Endpoint> endpoints, unsigned port);

// Bounded raw FujiBus accumulator. No scanning/resynchronization after any error.
// Caller owns partial-record deadlines and must retire the physical session on fault.
class RawPackets {
public:
    bool append(std::span<const std::uint8_t> bytes);
    bool pop(ByteBuffer& packet);
    bool faulted() const { return _fault; }
    bool partial() const { return !_partial.empty(); }
    bool ready() const { return !_ready.empty(); }
    std::uint64_t completed() const { return _completed; }
    void clear();
private:
    bool fail();
    bool _fault{false};
    std::uint64_t _completed{0};
    ByteBuffer _partial;
    std::deque<ByteBuffer> _ready;
};
} // namespace fujinet::io::visor_usb
