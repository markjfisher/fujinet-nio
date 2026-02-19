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

enum class HostType {
    Sd,
    Tnfs,
    // ... smb? http?
    Unknown,
};

struct HostConfig {
    int         id{0};           // stable logical id
    HostType    type{HostType::Unknown};
    std::string name;            // e.g. "SD", "fujinet.online"
    std::string address;         // TNFS hostname, path, etc.
    bool        enabled{true};
};

struct MountConfig {
    int         id{0};
    int         hostId{0};       // matches HostConfig.id
    std::string path;
    std::string mode{"r"};       // "r", "rw", etc.
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

    std::vector<HostConfig>  hosts;
    std::vector<MountConfig> mounts;

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
