#pragma once

#include <string>
#include <cstdint>

namespace fujinet::config {

enum class BootMode {
    Normal,
    Config,
    Unknown,
};

struct GeneralConfig {
    std::string deviceName;
};

struct BootConfig {
    BootMode    mode{BootMode::Config};
    std::string configUri{"persist:/boot/autorun.img"};
    bool        readOnly{true};
};

struct WifiConfig {
    bool        enabled{false};
    std::string ssid;
    std::string bssid;
    std::string passphrase;
};

struct ModemConfig {
    bool enabled{false};
    bool snifferEnabled{false};
};

struct CpmConfig {
    bool        enabled{false};
    std::string ccpImage;        // path or identifier
};

struct PrinterConfig {
    bool enabled{false};
    // add printer-specific fields later
};

struct NetSioConfig {
    bool        enabled{true};
    std::string host{"localhost"};
    std::uint16_t port{9997};
};

/// Atari physical SIO (ESP32 `SioGpio` boards), see docs/atari/hsio-report.md. The defaults are the values
/// the measurements there support; the gaps and the window can be tuned for a driver that needs more.
struct AtariSioConfig {
    int           hsioIndex{-1};              // high-speed SIO index answered to 3F (0-9; -1 = off)
    std::uint32_t ackGapUs{250};              // ACK -> COMPLETE gap, every speed
    std::uint32_t dataGapUs{0};               // COMPLETE -> data gap at high speed (SpartaDOS X may want 1000)
    std::uint32_t altWindowMs{10};            // how long a frame heard by the second listener stays usable
    bool          ignoreWriteGlitches{true};  // ignore CMD edges seen with the line high while a write waits
    bool          procNotify{false};          // pulse PROC when a network session becomes readable (docs/atari/proc.md)
};

struct ClockConfig {
    std::string timezone{"UTC"};  // POSIX timezone string, e.g. "UTC-8:45" or "CET-1CEST,M3.5.0,M10.5.0/3"
    bool        enabled{true};
};

/// Serial line format for ESP32 `UartGpio` / RS232 (and any future UART-backed channel).
enum class UartParity {
    None,
    Even,
    Odd,
};

enum class UartStopBits {
    One,
    OnePointFive,
    Two,
};

enum class UartFlowControl {
    None,
    RtsCts,
};

struct UartConfig {
    std::uint32_t     baudRate{115200};
    int               dataBits{8};  // 5..8
    UartParity        parity{UartParity::None};
    UartStopBits      stopBits{UartStopBits::One};
    UartFlowControl   flowControl{UartFlowControl::None};
    std::uint32_t     txGapUs{0};      // Delay once before the write; does not pace bytes.
    std::uint32_t     txByteGapUs{0};  // After each TX byte has left the UART, idle this many µs (0=off).
    std::uint32_t     txChunkSize{16}; // Chunk size in bytes (0=off). Ignored when txByteGapUs != 0.
    std::uint32_t     txChunkGapUs{2000}; // Idle after each full chunk except the last (0=off).
};

/// Packet link (docs/packet_link.md) settings, used by profiles with packetLink.
struct PacketLinkConfig {
    std::uint32_t capacity{0};        // Raw-packet capacity in bytes; 0 = default (4096).
    std::uint32_t recordTimeoutMs{0}; // Abandon a record after this gap; 0 = auto
                                      // (100 ms on a UART or serial port, 1000 ms on TCP).
};

/// Settings for the logical FujiBus channel (profile-dependent: PTY path on POSIX,
/// `uart` on ESP32 UartGpio, etc.).
struct ChannelConfig {
    std::string ptyPath{};  // POSIX PTY device path; empty = kernel-assigned
    std::string tcpHost{"127.0.0.1"}; // POSIX TCP serial bind/connect host
    std::uint16_t tcpPort{65504};      // POSIX TCP serial port for QEMU-style clients
    std::string serialPort{"/dev/ttyUSB0"}; // POSIX serial device for RS-232 profiles
    UartConfig  uart{};     // UART/RS-232 settings (baud, framing, optional RTS/CTS)
    PacketLinkConfig packetLink{};
};

/// Image content translation (docs/network_device_protocol.md).
struct ImageTranslationConfig {
    // Largest source image, as width*height, the Image translator decodes.
    // 0 = the platform's default (platform::default_image_max_pixels()).
    std::uint32_t maxPixels{0};
};

/// Content translation settings (`translation:`), one block per translator
/// that has any.
struct ContentTranslationConfig {
    ImageTranslationConfig image;
};

/// Palm HotSync (`hotsync:`), docs/hotsync.md. Read at start-up; a change
/// takes effect after a restart.
struct HotSyncConfig {
    bool          enabled{false};
    std::uint16_t port{14238};            // NetSync TCP port; 0 turns NetSync off
    // A Palm cradle shares the FujiBus serial line. After 60 s with no FujiBus
    // packet the line is deaf to FujiBus for 1.5 s of every 3 s, and requests
    // sent then go unanswered: only for a line whose clients retry.
    bool          cradle{false};
    std::string   root{"/hotsync"};       // folder on the persistent filesystem
    std::string   userName{"FujiNet"};    // given to a Palm that has never synced
    std::string   backup{"flagged"};      // none | flagged | all
};

// Unified config for the whole FujiNet instance.
struct FujiConfig {
    GeneralConfig        general;
    BootConfig           boot;
    WifiConfig           wifi;

    ModemConfig          modem;
    CpmConfig            cpm;
    PrinterConfig        printer;
    NetSioConfig         netsio;
    AtariSioConfig       atariSio;
    ClockConfig          clock;
    ChannelConfig        channel;
    ContentTranslationConfig translation;
    HotSyncConfig        hotsync;
};


// Abstract storage interface.
class FujiConfigStore {
public:
    virtual ~FujiConfigStore() = default;

    virtual FujiConfig load() = 0;
    virtual void       save(const FujiConfig& cfg) = 0;
};

} // namespace fujinet::config
