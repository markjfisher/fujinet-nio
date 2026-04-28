#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace fujinet::config {

enum class BootMode {
    Normal,
    Config,
    Cpm,
    Unknown,
};

struct GeneralConfig {
    std::string deviceName;
    BootMode    bootMode{BootMode::Config};
    std::string altConfigFile;
};

struct WifiConfig {
    bool        enabled{false};
    std::string ssid;
    std::string passphrase;
};

struct MountConfig {
    int         slot{0};            // Slot number (1-8). 0 means unassigned.
    std::string uri;                // URI of the resource to mount (e.g., "sd:/disks/img.ssd", "tnfs://server/dir/img.atr")
    std::string mode{"r"};          // "r", "rw", etc.
    bool        enabled{true};      // Whether this mount is active

    // Get effective slot index (0-7) or -1 if unassigned
    int effective_slot() const {
        if (slot >= 1 && slot <= 8) {
            return slot - 1;  // Convert 1-8 to 0-7
        }
        return -1;  // Unassigned
    }

    static constexpr int from_index(int index) {
        return (index >= 0 && index < 8) ? (index + 1) : 0;
    }
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

struct ClockConfig {
    std::string timezone{"UTC"};  // POSIX timezone string, e.g. "UTC-8:45" or "CET-1CEST,M3.5.0,M10.5.0/3"
    bool        enabled{true};
};

struct TlsConfig {
    bool trustTestCa{false};
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
};

/// Settings for the logical FujiBus channel (profile-dependent: PTY path on POSIX,
/// `uart` on ESP32 UartGpio, etc.).
struct ChannelConfig {
    std::string ptyPath{};  // POSIX PTY device path; empty = kernel-assigned
    UartConfig  uart{};     // ESP32 host UART (baud, framing, optional RTS/CTS)
};

// Unified config for the whole FujiNet instance.
struct FujiConfig {
    GeneralConfig        general;
    WifiConfig           wifi;

    std::vector<MountConfig> mounts;  // Mounted resources (URI-based)

    ModemConfig          modem;
    CpmConfig            cpm;
    PrinterConfig        printer;
    NetSioConfig         netsio;
    ClockConfig          clock;
    TlsConfig            tls;
    ChannelConfig        channel;
};


// Abstract storage interface.
class FujiConfigStore {
public:
    virtual ~FujiConfigStore() = default;

    virtual FujiConfig load() = 0;
    virtual void       save(const FujiConfig& cfg) = 0;
};

} // namespace fujinet::config
