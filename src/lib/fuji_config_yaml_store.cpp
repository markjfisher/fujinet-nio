#include "fujinet/config/fuji_config_yaml_store.h"
#include "fujinet/core/logging.h"

#include <fstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace fujinet::config {

static constexpr const char* TAG = "config_yaml";

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

static HostType parse_host_type(const std::string& s)
{
    if (s == "sd" || s == "SD")   return HostType::Sd;
    if (s == "tnfs" || s == "TNFS") return HostType::Tnfs;
    // others to go here, https? smb?
    return HostType::Unknown;
}

static std::string host_type_to_string(HostType t)
{
    switch (t) {
    case HostType::Sd:    return "SD";
    case HostType::Tnfs:  return "TNFS";
    default:              return "Unknown";
    }
}

// ---------- from_yaml helpers ----------

static void from_yaml(const YAML::Node& node, GeneralConfig& out)
{
    out.deviceName        = get_or<std::string>(node, "device_name", "");
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

static void from_yaml(const YAML::Node& node, HostConfig& out)
{
    out.id      = get_or<int>(node, "id", 0);
    out.name    = get_or<std::string>(node, "name", "");
    out.address = get_or<std::string>(node, "address", "");
    out.enabled = get_or<bool>(node, "enabled", true);

    auto typeStr = get_or<std::string>(node, "type", "Unknown");
    out.type     = parse_host_type(typeStr);
}

static void from_yaml(const YAML::Node& node, MountConfig& out)
{
    out.id     = get_or<int>(node, "id", 0);
    out.hostId = get_or<int>(node, "host_id", 0);
    out.path   = get_or<std::string>(node, "path", "");
    out.mode   = get_or<std::string>(node, "mode", "r");
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

// Top-level FujiConfig mapper.
static void from_yaml(const YAML::Node& root, FujiConfig& cfg)
{
    if (auto n = root["fujinet"]) {
        from_yaml(n, cfg.general);
    }

    if (auto n = root["wifi"]) {
        from_yaml(n, cfg.wifi);
    }

    if (auto hosts = root["hosts"]; hosts && hosts.IsSequence()) {
        cfg.hosts.clear();
        for (const auto& hn : hosts) {
            HostConfig hc{};
            from_yaml(hn, hc);
            cfg.hosts.push_back(std::move(hc));
        }
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

    // hosts:
    out << YAML::Key << "hosts" << YAML::Value << YAML::BeginSeq;
    for (const auto& h : cfg.hosts) {
        out << YAML::BeginMap;
        out << YAML::Key << "id"      << YAML::Value << h.id;
        out << YAML::Key << "name"    << YAML::Value << h.name;
        out << YAML::Key << "address" << YAML::Value << h.address;
        out << YAML::Key << "enabled" << YAML::Value << h.enabled;
        out << YAML::Key << "type"    << YAML::Value << host_type_to_string(h.type);
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;

    // mounts:
    out << YAML::Key << "mounts" << YAML::Value << YAML::BeginSeq;
    for (const auto& m : cfg.mounts) {
        out << YAML::BeginMap;
        out << YAML::Key << "id"      << YAML::Value << m.id;
        out << YAML::Key << "host_id" << YAML::Value << m.hostId;
        out << YAML::Key << "path"    << YAML::Value << m.path;
        out << YAML::Key << "mode"    << YAML::Value << m.mode;
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

    out << YAML::EndMap; // root
}

// ---------- YamlFujiConfigStore methods ----------

YamlFujiConfigStore::YamlFujiConfigStore(std::string path)
    : _path(std::move(path))
{
}

FujiConfig YamlFujiConfigStore::load()
{
    FujiConfig cfg;  // defaults

    std::ifstream in(_path);
    if (!in.is_open()) {
        FN_LOGW(TAG, "Config file '%s' not found, writing defaults.", _path.c_str());
        save(cfg);
        return cfg;
    }

    try {
        YAML::Node root = YAML::Load(in);
        from_yaml(root, cfg);
        FN_LOGI(TAG, "Loaded config from '%s'.", _path.c_str());
    } catch (const std::exception& e) {
        FN_LOGE(TAG, "Failed to parse config file '%s': %s",
                _path.c_str(), e.what());
        // Keep defaults.
    }

    return cfg;
}


void YamlFujiConfigStore::save(const FujiConfig& cfg)
{
    YAML::Emitter out;
    to_yaml(out, cfg);

    std::ofstream f(_path, std::ios::trunc);
    if (!f.is_open()) {
        FN_LOGE(TAG, "Failed to open config file '%s' for write.", _path.c_str());
        return;
    }

    f << out.c_str();
    FN_LOGI(TAG, "Saved config to '%s'.", _path.c_str());
}

} // namespace fujinet::config
