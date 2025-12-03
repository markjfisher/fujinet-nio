#pragma once

#include <string_view>

namespace fujinet::config {

enum class Machine {
    Generic,
    FujiNetESP32,
    Atari8Bit,
    C64,
    Apple2,
    FujiNetPi,
    // ...
};

// Logical protocol / framing on the link.
enum class TransportKind {
    FujiBus,        // SLIP + FujiBus headers, the standard transport
    SIO,            // legacy SIO transport (future)
    IEC,            // C64 IEC bus (future)
    // ...
};

// Physical / OS-level mechanism for moving bytes.
enum class ChannelKind {
    Pty,            // POSIX pseudo-terminal, for dev/debug
    UsbCdcDevice,   // device-side USB CDC (ESP32 TinyUSB, later Pi gadget)
    TcpSocket,      // future: TCP-based link for emulators
    // add RealTty, HardwareSio, etc later if needed
};

struct BuildProfile {
    Machine       machine;
    TransportKind primaryTransport;
    ChannelKind   primaryChannel;
    std::string_view name;
};

// One global build-time profile.
BuildProfile current_build_profile();

} // namespace fujinet::config
