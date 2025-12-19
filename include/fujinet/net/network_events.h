#pragma once

#include <cstdint>
#include <string>

namespace fujinet::net {

enum class NetworkEventKind : std::uint8_t {
    LinkUp,
    GotIp,
    LinkDown,
};

struct NetworkGotIp {
    std::string ip4;      // "192.168.1.130"
    std::string netmask;  // optional (may be empty)
    std::string gateway;  // optional (may be empty)
};

struct NetworkEvent {
    NetworkEventKind kind{NetworkEventKind::LinkDown};
    NetworkGotIp gotIp{}; // valid only when kind == GotIp
};

} // namespace fujinet::net
