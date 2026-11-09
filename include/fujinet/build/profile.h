#pragma once

#include <string_view>
#include <cstddef>
#include <cstdint>

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
enum class TransportKind {
    None,           // physical ingress owns a non-FujiBus protocol plane
    FujiBusSlip,   // FujiBus over SLIP framing (byte-stream channels)
    FujiBusNative, // FujiBus over packet-native channels (Zorro, SPI, floppy/Pico)
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
    UdpSocket,    // UDP socket (for NetSIO protocol)
    UartGpio,     // ESP32 GPIO-based UART (RS232, etc.)
    SioGpio,      // ESP32 Atari SIO connector owned by the dual-plane ingress
    SerialPort,   // POSIX serial port (RS-232 via /dev/ttyS* or /dev/ttyUSB*)
    // Future: SpiBridge, etc.
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
    // Carry FujiBusNative packets over a packet link (docs/packet_link.md):
    // for a byte-stream channel such as a UART from a bridge microcontroller.
    bool             packetLink{false};
    std::string_view name;

    HardwareCapabilities hw;

    // Where this build expects its boot/config disk (the FujiConfig boot.config_uri
    // default). Empty: the generic default. A fujinet.yaml that sets config_uri wins.
    std::string_view defaultBootConfigUri{};

    // The channel.tcp_port this build listens on by default. 0: the generic default.
    // A fujinet.yaml that sets tcp_port wins.
    std::uint16_t defaultChannelTcpPort{0};

    // The channel.uart baud rate this build starts at, for a UART whose far end the
    // board fixes, such as a link controller. 0: the generic default. A fujinet.yaml
    // that sets baud_rate wins.
    std::uint32_t defaultUartBaudRate{0};

    // Start with no TX pacing on the UART (tx_chunk_size and tx_chunk_gap_us 0), for
    // a far end that reads at line rate. A fujinet.yaml that sets either wins.
    bool defaultUartUnpaced{false};
};

// Filled by platform-specific code.
HardwareCapabilities detect_hardware_capabilities();

// Global build-time/runtime profile.
BuildProfile current_build_profile();

} // namespace fujinet::build
