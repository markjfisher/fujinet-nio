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
    int         slot{0};            // Slot number (1-8). If not specified, falls back to legacy `id`.
    std::string uri;              // URI of the resource to mount (e.g., "sd:/disks/img.ssd", "tnfs://server/dir/img.atr")
    std::string mode{"r"};       // "r", "rw", etc.
    bool        enabled{true};    // Whether this mount is active
    int         id{0};            // Legacy field for backward compatibility with old YAML configs

    // Backward compatibility: get effective slot (prefer explicit slot, fallback to legacy id)
    // Returns 0-based slot index, or -1 if unassigned
    int effective_slot() const {
        if (slot >= 1 && slot <= 8) {
            return slot - 1;  // Convert 1-8 to 0-7
        }
        if (id >= 1 && id <= 8) {
            return id - 1;  // Convert legacy id 1-8 to 0-7
        }
        return -1;  // Unassigned
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
};


// Abstract storage interface.
class FujiConfigStore {
public:
    virtual ~FujiConfigStore() = default;

    virtual FujiConfig load() = 0;
    virtual void       save(const FujiConfig& cfg) = 0;
};

} // namespace fujinet::config
