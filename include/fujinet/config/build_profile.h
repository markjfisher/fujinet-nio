#pragma once

#include <string_view>

namespace fujinet::config {

enum class Machine {
    Generic,
    Atari8Bit,
    C64,
    Apple2,
    // ...
};

enum class TransportKind {
    RS232,
    SIO,
    IEC,
    PTY,      // POSIX pseudo-terminal
    // ...
};

struct BuildProfile {
    Machine       machine;
    TransportKind primaryTransport;
    std::string_view name;
};

// One global build-time profile.
BuildProfile current_build_profile();

} // namespace fujinet::config
