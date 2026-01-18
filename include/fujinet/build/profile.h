#pragma once

#include <string_view>
#include <cstddef>

namespace fujinet::build {

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
// (FujiBus is SLIP + FujiBus header framing)
enum class TransportKind {
    FujiBus,
    SIO,
    IWM,
    IEC,
    // ...
};

// Physical / OS-level byte transport mechanism.
enum class ChannelKind {
    Pty,
    UsbCdcDevice,
    TcpSocket,
    // Future: HardwareSio, RealTty, SpiBridge, etc.
};

// --------------------------
//  Hardware Capabilities
// --------------------------

struct NetworkCapabilities {
    bool hasLocalNetwork{false};        // Can open TCP/UDP sockets at all
    bool managesItsOwnLink{false};      // ESP32 manages WiFi via on-device config
    bool supportsAccessPointMode{false}; // SoftAP for config portals
};

struct MemoryCapabilities {
    std::size_t persistentStorageBytes{0}; // FS capacity (flash partition, disk, ...)
    std::size_t largeMemoryPoolBytes{0};   // Big RAM pool (e.g., PSRAM)
    bool hasDedicatedLargePool{false};     // True if separate from main RAM (ESP32 PSRAM)
};

struct HardwareCapabilities {
    NetworkCapabilities network;
    MemoryCapabilities  memory;

    bool hasUsbDevice{false};
    bool hasUsbHost{false};
};

// ------------------------

struct BuildProfile {
    Machine          machine;
    TransportKind    primaryTransport;
    ChannelKind      primaryChannel;
    std::string_view name;

    HardwareCapabilities hw;
};

// Filled by platform-specific code.
HardwareCapabilities detect_hardware_capabilities();

// Global build-time/runtime profile.
BuildProfile current_build_profile();

} // namespace fujinet::build
