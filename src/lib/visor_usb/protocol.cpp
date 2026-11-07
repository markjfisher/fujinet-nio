#include "fujinet/io/visor_usb/protocol.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"

namespace fujinet::io::visor_usb {
std::optional<std::vector<LogicalPort>> connection_info(std::span<const std::uint8_t> b)
{
    if (b.size() != 6 || b[1] != 0 || b[0] < 1 || b[0] > 2) return {};
    std::vector<LogicalPort> ports;
    for (unsigned i = 0; i < b[0]; ++i) {
        const unsigned function = b[2 + i * 2], port = b[3 + i * 2];
        if (function > 4 || port < 1 || port > 2) return {};
        for (const auto& p : ports) if (p.port == port) return {};
        ports.push_back({function, port});
    }
    return ports;
}
std::optional<EndpointPair> select_generic_pair(std::span<const LogicalPort> ports,
    std::span<const Endpoint> eps, unsigned port)
{
    if (port != 1 && port != 2) return {};
    unsigned generic = 0;
    for (const auto& p : ports) if (p.port == port && p.function == 0) ++generic;
    if (generic != 1) return {};
    const auto validMps = [](unsigned n) { return n == 8 || n == 16 || n == 32 || n == 64; };
    std::optional<EndpointPair> found;
    for (const auto& in : eps) {
        if (in.configuration != 1 || in.alternate != 0 || in.interfaceClass != 255 ||
            in.address != (0x80 | port) || in.type != 2 || !validMps(in.mps)) continue;
        for (const auto& out : eps) {
            if (out.configuration != in.configuration || out.interface != in.interface ||
                out.alternate != in.alternate || out.interfaceClass != in.interfaceClass ||
                out.address != port || out.type != 2 || !validMps(out.mps)) continue;
            if (found) return {}; // no first-match or ambiguous endpoint selection
            found = EndpointPair{in, out};
        }
    }
    return found;
}
bool RawPackets::fail() { clear(); _fault = true; return false; }
void RawPackets::clear() { _partial.clear(); _ready.clear(); _fault = false; _completed = 0; }
bool RawPackets::append(std::span<const std::uint8_t> bytes)
{
    if (_fault) return false;
    for (auto b : bytes) {
        _partial.push_back(b);
        if (_partial.size() < 4) continue;
        const std::size_t n = _partial[2] | (std::size_t(_partial[3]) << 8);
        if (n < 6 || n > packet_capacity) return fail();
        if (_partial.size() == n) {
            if (!protocol::FujiBusPacket::fromRaw(_partial) || _ready.size() >= 4) return fail();
            ++_completed;
            _ready.push_back(std::move(_partial));
            _partial.clear();
        }
    }
    return true;
}
bool RawPackets::pop(ByteBuffer& packet)
{
    if (_fault || _ready.empty()) return false;
    packet = std::move(_ready.front()); _ready.pop_front(); return true;
}
} // namespace fujinet::io::visor_usb
