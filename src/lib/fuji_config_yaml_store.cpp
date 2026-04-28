#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/core/logging.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace fujinet::config {

using fujinet::log::Level;
static constexpr const char* TAG = "config";

// ---------- tiny helpers ----------

template<typename T>
static T get_or(const YAML::Node& obj, const char* key, T def)
{
    auto n = obj[key];
    return n ? n.as<T>() : def;
}

static BootMode parse_boot_mode(const std::string& s)
{
    if (s == "normal") return BootMode::Normal;
    if (s == "config") return BootMode::Config;
    if (s == "cpm")    return BootMode::Cpm;
    return BootMode::Unknown;
}

static std::string boot_mode_to_string(BootMode m)
{
    switch (m) {
    case BootMode::Normal: return "normal";
    case BootMode::Config: return "config";
    case BootMode::Cpm:    return "cpm";
    default:               return "unknown";
    }
}

// ---------- from_yaml helpers ----------

static void from_yaml(const YAML::Node& node, GeneralConfig& out)
{
    out.deviceName        = get_or<std::string>(node, "device_name", "fujinet");
    auto bootModeStr      = get_or<std::string>(node, "boot_mode", "normal");
    out.bootMode          = parse_boot_mode(bootModeStr);
    out.altConfigFile     = get_or<std::string>(node, "alt_config_file", "");
}

static void from_yaml(const YAML::Node& node, WifiConfig& out)
{
    out.enabled    = get_or<bool>(node, "enabled", false);
    out.ssid       = get_or<std::string>(node, "ssid", "");
    out.passphrase = get_or<std::string>(node, "passphrase", "");
}

static void from_yaml(const YAML::Node& node, MountConfig& out)
{
    out.slot    = get_or<int>(node, "slot", 0);
    out.uri     = get_or<std::string>(node, "uri", "");
    out.mode    = get_or<std::string>(node, "mode", "r");
    out.enabled = get_or<bool>(node, "enabled", true);
}

static void from_yaml(const YAML::Node& node, ModemConfig& out)
{
    out.enabled        = get_or<bool>(node, "enabled", false);
    out.snifferEnabled = get_or<bool>(node, "sniffer_enabled", false);
}

static void from_yaml(const YAML::Node& node, CpmConfig& out)
{
    out.enabled   = get_or<bool>(node, "enabled", false);
    out.ccpImage  = get_or<std::string>(node, "ccp_image", "");
}

static void from_yaml(const YAML::Node& node, PrinterConfig& out)
{
    out.enabled = get_or<bool>(node, "enabled", false);
}

static void from_yaml(const YAML::Node& node, NetSioConfig& out)
{
    out.enabled = get_or<bool>(node, "enabled", false);
    out.host    = get_or<std::string>(node, "host", "localhost");
    out.port    = static_cast<std::uint16_t>(get_or<int>(node, "port", 9997));
}

static void from_yaml(const YAML::Node& node, ClockConfig& out)
{
    out.timezone = get_or<std::string>(node, "timezone", "UTC");
    out.enabled  = get_or<bool>(node, "enabled", true);
}

static void from_yaml(const YAML::Node& node, TlsConfig& out)
{
    out.trustTestCa = get_or<bool>(node, "trust_test_ca", false);
}

static std::string yaml_lower_ascii(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static UartParity parse_uart_parity(const std::string& raw)
{
    const std::string s = yaml_lower_ascii(raw);
    if (s == "even") {
        return UartParity::Even;
    }
    if (s == "odd") {
        return UartParity::Odd;
    }
    return UartParity::None;
}

static UartStopBits parse_uart_stop_bits(const std::string& raw)
{
    const std::string s = yaml_lower_ascii(raw);
    if (s == "2" || s == "two") {
        return UartStopBits::Two;
    }
    if (s == "1.5" || s == "1_5" || s == "one_point_five") {
        return UartStopBits::OnePointFive;
    }
    return UartStopBits::One;
}

static UartFlowControl parse_uart_flow_control(const std::string& raw)
{
    const std::string s = yaml_lower_ascii(raw);
    if (s == "rts_cts" || s == "rts-cts" || s == "hw") {
        return UartFlowControl::RtsCts;
    }
    return UartFlowControl::None;
}

static std::string uart_parity_to_string(UartParity p)
{
    switch (p) {
    case UartParity::Even:
        return "even";
    case UartParity::Odd:
        return "odd";
    case UartParity::None:
    default:
        return "none";
    }
}

static std::string uart_stop_bits_to_string(UartStopBits s)
{
    switch (s) {
    case UartStopBits::Two:
        return "2";
    case UartStopBits::OnePointFive:
        return "1.5";
    case UartStopBits::One:
    default:
        return "1";
    }
}

static std::string uart_flow_to_string(UartFlowControl f)
{
    switch (f) {
    case UartFlowControl::RtsCts:
        return "rts_cts";
    case UartFlowControl::None:
    default:
        return "none";
    }
}

static void from_yaml(const YAML::Node& node, UartConfig& out)
{
    out.baudRate   = static_cast<std::uint32_t>(get_or<int>(node, "baud_rate", 115200));
    out.dataBits   = get_or<int>(node, "data_bits", 8);
    out.parity     = parse_uart_parity(get_or<std::string>(node, "parity", "none"));
    out.stopBits   = parse_uart_stop_bits(get_or<std::string>(node, "stop_bits", "1"));
    out.flowControl = parse_uart_flow_control(get_or<std::string>(node, "flow_control", "none"));
}

static void from_yaml(const YAML::Node& node, ChannelConfig& out)
{
    out.ptyPath = get_or<std::string>(node, "pty_path", "");
    if (auto u = node["uart"]) {
        if (u.IsMap()) {
            from_yaml(u, out.uart);
        }
    }
    // Legacy flat key (still honored so older configs work)
    if (node["uart_baud"]) {
        out.uart.baudRate = static_cast<std::uint32_t>(node["uart_baud"].as<int>());
    }
}

// Top-level FujiConfig mapper.
static void from_yaml(const YAML::Node& root, FujiConfig& cfg)
{
    if (auto n = root["fujinet"]) {
        from_yaml(n, cfg.general);
    }

    if (auto n = root["wifi"]) {
        from_yaml(n, cfg.wifi);
    }

    if (auto mounts = root["mounts"]; mounts && mounts.IsSequence()) {
        cfg.mounts.clear();
        for (const auto& mn : mounts) {
            MountConfig mc{};
            from_yaml(mn, mc);
            cfg.mounts.push_back(std::move(mc));
        }
    }

    if (auto devs = root["devices"]) {
        if (auto n = devs["modem"])   from_yaml(n, cfg.modem);
        if (auto n = devs["cpm"])     from_yaml(n, cfg.cpm);
        if (auto n = devs["printer"]) from_yaml(n, cfg.printer);
    }
    
    if (auto n = root["netsio"]) {
        from_yaml(n, cfg.netsio);
    }
    
    if (auto n = root["clock"]) {
        from_yaml(n, cfg.clock);
    }

    if (auto n = root["tls"]) {
        from_yaml(n, cfg.tls);
    }

    if (auto n = root["channel"]) {
        from_yaml(n, cfg.channel);
    }
}

// ---------- to_yaml helpers (minimal stub to start) ----------

static void to_yaml(YAML::Emitter& out, const FujiConfig& cfg)
{
    out << YAML::BeginMap;

    // fujinet:
    out << YAML::Key << "fujinet" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "device_name"        << YAML::Value << cfg.general.deviceName;
    out << YAML::Key << "boot_mode"          << YAML::Value << boot_mode_to_string(cfg.general.bootMode);
    out << YAML::Key << "alt_config_file"    << YAML::Value << cfg.general.altConfigFile;
    out << YAML::EndMap;

    // wifi:
    out << YAML::Key << "wifi" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled"    << YAML::Value << cfg.wifi.enabled;
    out << YAML::Key << "ssid"       << YAML::Value << cfg.wifi.ssid;
    out << YAML::Key << "passphrase" << YAML::Value << cfg.wifi.passphrase;
    out << YAML::EndMap;

    // mounts:
    out << YAML::Key << "mounts" << YAML::Value << YAML::BeginSeq;
    for (const auto& m : cfg.mounts) {
        out << YAML::BeginMap;
        if (m.slot >= 1 && m.slot <= 8) {
            out << YAML::Key << "slot" << YAML::Value << m.slot;
        }
        out << YAML::Key << "uri"     << YAML::Value << m.uri;
        out << YAML::Key << "mode"    << YAML::Value << m.mode;
        out << YAML::Key << "enabled" << YAML::Value << m.enabled;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;

    // devices:
    out << YAML::Key << "devices" << YAML::Value << YAML::BeginMap;

    out << YAML::Key << "modem" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled"        << YAML::Value << cfg.modem.enabled;
    out << YAML::Key << "sniffer_enabled"<< YAML::Value << cfg.modem.snifferEnabled;
    out << YAML::EndMap;

    out << YAML::Key << "cpm" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled"   << YAML::Value << cfg.cpm.enabled;
    out << YAML::Key << "ccp_image" << YAML::Value << cfg.cpm.ccpImage;
    out << YAML::EndMap;

    out << YAML::Key << "printer" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled" << YAML::Value << cfg.printer.enabled;
    out << YAML::EndMap;

    out << YAML::EndMap; // devices

    // netsio:
    out << YAML::Key << "netsio" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled" << YAML::Value << cfg.netsio.enabled;
    out << YAML::Key << "host"    << YAML::Value << cfg.netsio.host;
    out << YAML::Key << "port"    << YAML::Value << cfg.netsio.port;
    out << YAML::EndMap;

     // clock:
     out << YAML::Key << "clock" << YAML::Value << YAML::BeginMap;
     out << YAML::Key << "timezone" << YAML::Value << cfg.clock.timezone;
     out << YAML::Key << "enabled"  << YAML::Value << cfg.clock.enabled;
     out << YAML::EndMap;

     // tls:
     out << YAML::Key << "tls" << YAML::Value << YAML::BeginMap;
     out << YAML::Key << "trust_test_ca" << YAML::Value << cfg.tls.trustTestCa;
     out << YAML::EndMap;

      // channel:
      out << YAML::Key << "channel" << YAML::Value << YAML::BeginMap;
     out << YAML::Key << "pty_path" << YAML::Value << cfg.channel.ptyPath;
     out << YAML::Key << "uart" << YAML::Value << YAML::BeginMap;
     out << YAML::Key << "baud_rate"     << YAML::Value << cfg.channel.uart.baudRate;
     out << YAML::Key << "data_bits"     << YAML::Value << cfg.channel.uart.dataBits;
     out << YAML::Key << "parity"        << YAML::Value << uart_parity_to_string(cfg.channel.uart.parity);
     out << YAML::Key << "stop_bits"     << YAML::Value << uart_stop_bits_to_string(cfg.channel.uart.stopBits);
     out << YAML::Key << "flow_control"  << YAML::Value << uart_flow_to_string(cfg.channel.uart.flowControl);
     out << YAML::EndMap; // uart
     out << YAML::EndMap;

    out << YAML::EndMap; // root
}

// --- small local helpers using IFile ---

static std::string read_all(fs::IFile& file)
{
    std::string out;
    std::vector<std::uint8_t> buf(1024);

    for (;;) {
        std::size_t n = file.read(buf.data(), buf.size());
        if (n == 0) {
            break;
        }
        out.append(reinterpret_cast<const char*>(buf.data()), n);
    }
    return out;
}

static void write_all(fs::IFile& file, const std::string& data)
{
    const auto* ptr = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t remaining = data.size();

    while (remaining > 0) {
        std::size_t written = file.write(ptr, remaining);
        if (written == 0) {
            throw std::runtime_error("short write while saving config");
        }
        remaining -= written;
        ptr       += written;
    }
    (void)file.flush();
}

// ---------- YamlFujiConfigStoreFs methods ----------

YamlFujiConfigStoreFs::YamlFujiConfigStoreFs(fs::IFileSystem* primary,
                                             fs::IFileSystem* backup,
                                             std::string      relativePath)
    : _primary(primary)
    , _backup(backup)
    , _relPath(std::move(relativePath))
{
}

FujiConfig YamlFujiConfigStoreFs::load()
{
    FujiConfig cfg{}; // defaults

    // Try primary first if present
    if (_primary && _primary->exists(_relPath)) {
        try {
            return loadFromFs(*_primary);
        } catch (const std::exception& ex) {
            FN_LOGE(TAG,
                    "Failed to load config from primary '%s' on '%s': %s",
                    _relPath.c_str(),
                    _primary->name().c_str(),
                    ex.what());
        }
    }

    // Then backup
    if (_backup && _backup->exists(_relPath)) {
        try {
            auto config = loadFromFs(*_backup);
            if (_primary) {
                FN_LOGI(TAG, "copying missing config from backup to primary");
                save(config);
            }
            return config;
        } catch (const std::exception& ex) {
            FN_LOGE(TAG,
                    "Failed to load config from backup '%s' on '%s': %s",
                    _relPath.c_str(),
                    _backup->name().c_str(),
                    ex.what());
        }
    }

    // Nothing found anywhere: write defaults so the file exists for next boot.
    FN_LOGW(TAG,
            "Config '%s' not found on any filesystem; writing defaults",
            _relPath.c_str());

    try {
        save(cfg);
    } catch (const std::exception& ex) {
        FN_LOGE(TAG,
                "Failed to write default config '%s': %s",
                _relPath.c_str(),
                ex.what());
    }

    return cfg;
}

void YamlFujiConfigStoreFs::save(const FujiConfig& cfg)
{
    // primary is the "authoritative" copy
    if (_primary) {
        try {
            saveToFs(*_primary, cfg);
        } catch (const std::exception& ex) {
            FN_LOGE(TAG,
                    "Failed to save config to primary '%s' on '%s': %s",
                    _relPath.c_str(),
                    _primary->name().c_str(),
                    ex.what());
        }
    }

    // backup is a best-effort mirror
    if (_backup) {
        try {
            saveToFs(*_backup, cfg);
        } catch (const std::exception& ex) {
            FN_LOGE(TAG,
                    "Failed to save config to backup '%s' on '%s': %s",
                    _relPath.c_str(),
                    _backup->name().c_str(),
                    ex.what());
        }
    }
}

FujiConfig YamlFujiConfigStoreFs::loadFromFs(fs::IFileSystem& fs)
{
    auto file = fs.open(_relPath, "rb");
    if (!file) {
        throw std::runtime_error("open for read failed");
    }

    std::string yamlText = read_all(*file);
    if (yamlText.empty()) {
        FN_LOGW(TAG,
                "Config '%s' on '%s' is empty; using defaults",
                _relPath.c_str(), fs.name().c_str());
        return FujiConfig{};
    }

    YAML::Node root = YAML::Load(yamlText);

    FujiConfig cfg{};
    // <== reuse existing mapping helper:
    from_yaml(root, cfg);

    FN_ELOG("Loaded config from '%s' on '%s'",
            _relPath.c_str(), fs.name().c_str());
    return cfg;
}

void YamlFujiConfigStoreFs::saveToFs(fs::IFileSystem& fs, const FujiConfig& cfg)
{
    YAML::Emitter out;
    to_yaml(out, cfg);

    auto file = fs.open(_relPath, "wb");
    if (!file) {
        throw std::runtime_error("open for write failed");
    }

    const std::string text = out.c_str();
    write_all(*file, text);

    FN_LOGI(TAG,
            "Saved config to '%s' on '%s'",
            _relPath.c_str(), fs.name().c_str());
}

} // namespace fujinet::config
