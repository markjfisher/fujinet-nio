#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"

#include <cstdint>
#include <string>
#include <vector>

using fujinet::config::BootMode;
using fujinet::config::FujiConfig;
using fujinet::config::HostConfig;
using fujinet::config::HostType;
using fujinet::config::MountConfig;
using fujinet::config::YamlFujiConfigStoreFs;

namespace {

// Helper to create a file with content in a MemoryFileSystem
void create_file(fujinet::tests::MemoryFileSystem& fs, const std::string& path, const std::string& content)
{
    std::vector<std::uint8_t> bytes(content.begin(), content.end());
    fs.create_file(path, bytes);
}

// Helper to read file content from a MemoryFileSystem
std::string read_file(fujinet::tests::MemoryFileSystem& fs, const std::string& path)
{
    auto file = fs.open(path, "rb");
    if (!file) return "";
    std::vector<std::uint8_t> buf(4096);
    std::string result;
    for (;;) {
        std::size_t n = file->read(buf.data(), buf.size());
        if (n == 0) break;
        result.append(reinterpret_cast<const char*>(buf.data()), n);
    }
    return result;
}

// Helper to compare configs
bool configs_equal(const FujiConfig& a, const FujiConfig& b)
{
    if (a.general.deviceName != b.general.deviceName) return false;
    if (a.general.bootMode != b.general.bootMode) return false;
    if (a.general.altConfigFile != b.general.altConfigFile) return false;

    if (a.wifi.enabled != b.wifi.enabled) return false;
    if (a.wifi.ssid != b.wifi.ssid) return false;
    if (a.wifi.passphrase != b.wifi.passphrase) return false;

    if (a.hosts.size() != b.hosts.size()) return false;
    for (std::size_t i = 0; i < a.hosts.size(); ++i) {
        if (a.hosts[i].id != b.hosts[i].id) return false;
        if (a.hosts[i].type != b.hosts[i].type) return false;
        if (a.hosts[i].name != b.hosts[i].name) return false;
        if (a.hosts[i].address != b.hosts[i].address) return false;
        if (a.hosts[i].enabled != b.hosts[i].enabled) return false;
    }

    if (a.mounts.size() != b.mounts.size()) return false;
    for (std::size_t i = 0; i < a.mounts.size(); ++i) {
        if (a.mounts[i].id != b.mounts[i].id) return false;
        if (a.mounts[i].hostId != b.mounts[i].hostId) return false;
        if (a.mounts[i].path != b.mounts[i].path) return false;
        if (a.mounts[i].mode != b.mounts[i].mode) return false;
    }

    if (a.modem.enabled != b.modem.enabled) return false;
    if (a.modem.snifferEnabled != b.modem.snifferEnabled) return false;

    if (a.cpm.enabled != b.cpm.enabled) return false;
    if (a.cpm.ccpImage != b.cpm.ccpImage) return false;

    if (a.printer.enabled != b.printer.enabled) return false;

    return true;
}

} // namespace

TEST_CASE("YamlFujiConfigStoreFs: Load minimal config from primary")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    // Minimal valid YAML config
    const std::string yaml = R"(
fujinet:
  device_name: "test-device"
  boot_mode: "normal"
  alt_config_file: ""
wifi:
  enabled: false
  ssid: ""
  passphrase: ""
hosts: []
mounts: []
devices:
  modem:
    enabled: false
    sniffer_enabled: false
  cpm:
    enabled: false
    ccp_image: ""
  printer:
    enabled: false
)";

    create_file(*primary, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    CHECK(cfg.general.deviceName == "test-device");
    CHECK(cfg.general.bootMode == BootMode::Normal);
    CHECK(cfg.general.altConfigFile == "");
    CHECK(cfg.wifi.enabled == false);
    CHECK(cfg.hosts.empty());
    CHECK(cfg.mounts.empty());
}

TEST_CASE("YamlFujiConfigStoreFs: Load full config from primary")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    // Full YAML config with all fields populated
    const std::string yaml = R"(
fujinet:
  device_name: "my-fujinet"
  boot_mode: "config"
  alt_config_file: "/alt/config.yaml"
wifi:
  enabled: true
  ssid: "MyWiFi"
  passphrase: "secret123"
hosts:
  - id: 1
    name: "SD Card"
    address: "/sd"
    enabled: true
    type: "SD"
  - id: 2
    name: "TNFS Server"
    address: "fujinet.online"
    enabled: true
    type: "TNFS"
  - id: 3
    name: "Disabled Host"
    address: "example.com"
    enabled: false
    type: "TNFS"
mounts:
  - id: 1
    host_id: 1
    path: "/disks"
    mode: "rw"
  - id: 2
    host_id: 2
    path: "/atari"
    mode: "r"
devices:
  modem:
    enabled: true
    sniffer_enabled: true
  cpm:
    enabled: true
    ccp_image: "/cpm/ccp.img"
  printer:
    enabled: true
)";

    create_file(*primary, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    CHECK(cfg.general.deviceName == "my-fujinet");
    CHECK(cfg.general.bootMode == BootMode::Config);
    CHECK(cfg.general.altConfigFile == "/alt/config.yaml");

    CHECK(cfg.wifi.enabled == true);
    CHECK(cfg.wifi.ssid == "MyWiFi");
    CHECK(cfg.wifi.passphrase == "secret123");

    REQUIRE(cfg.hosts.size() == 3);
    CHECK(cfg.hosts[0].id == 1);
    CHECK(cfg.hosts[0].name == "SD Card");
    CHECK(cfg.hosts[0].address == "/sd");
    CHECK(cfg.hosts[0].enabled == true);
    CHECK(cfg.hosts[0].type == HostType::Sd);

    CHECK(cfg.hosts[1].id == 2);
    CHECK(cfg.hosts[1].name == "TNFS Server");
    CHECK(cfg.hosts[1].address == "fujinet.online");
    CHECK(cfg.hosts[1].enabled == true);
    CHECK(cfg.hosts[1].type == HostType::Tnfs);

    CHECK(cfg.hosts[2].id == 3);
    CHECK(cfg.hosts[2].enabled == false);
    CHECK(cfg.hosts[2].type == HostType::Tnfs);

    REQUIRE(cfg.mounts.size() == 2);
    CHECK(cfg.mounts[0].id == 1);
    CHECK(cfg.mounts[0].hostId == 1);
    CHECK(cfg.mounts[0].path == "/disks");
    CHECK(cfg.mounts[0].mode == "rw");

    CHECK(cfg.mounts[1].id == 2);
    CHECK(cfg.mounts[1].hostId == 2);
    CHECK(cfg.mounts[1].path == "/atari");
    CHECK(cfg.mounts[1].mode == "r");

    CHECK(cfg.modem.enabled == true);
    CHECK(cfg.modem.snifferEnabled == true);

    CHECK(cfg.cpm.enabled == true);
    CHECK(cfg.cpm.ccpImage == "/cpm/ccp.img");

    CHECK(cfg.printer.enabled == true);
}

TEST_CASE("YamlFujiConfigStoreFs: Load from backup when primary missing")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");
    auto backup = std::make_unique<fujinet::tests::MemoryFileSystem>("backup");

    const std::string yaml = R"(
fujinet:
  device_name: "backup-device"
  boot_mode: "normal"
  alt_config_file: ""
wifi:
  enabled: false
  ssid: ""
  passphrase: ""
hosts: []
mounts: []
devices:
  modem:
    enabled: false
    sniffer_enabled: false
  cpm:
    enabled: false
    ccp_image: ""
  printer:
    enabled: false
)";

    // Only backup has the file
    create_file(*backup, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), backup.get(), "fujinet.yaml");
    FujiConfig cfg = store.load();

    CHECK(cfg.general.deviceName == "backup-device");
    CHECK(cfg.general.bootMode == BootMode::Normal);

    // Should have been copied to primary
    CHECK(primary->exists("fujinet.yaml"));
    std::string primaryContent = read_file(*primary, "fujinet.yaml");
    CHECK(!primaryContent.empty());
}

TEST_CASE("YamlFujiConfigStoreFs: Load from backup when primary fails")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");
    auto backup = std::make_unique<fujinet::tests::MemoryFileSystem>("backup");

    const std::string validYaml = R"(
fujinet:
  device_name: "backup-device"
  boot_mode: "normal"
  alt_config_file: ""
wifi:
  enabled: false
  ssid: ""
  passphrase: ""
hosts: []
mounts: []
devices:
  modem:
    enabled: false
    sniffer_enabled: false
  cpm:
    enabled: false
    ccp_image: ""
  printer:
    enabled: false
)";

    // Primary has invalid YAML, backup has valid
    create_file(*primary, "/fujinet.yaml", "invalid: yaml: [");
    create_file(*backup, "/fujinet.yaml", validYaml);

    YamlFujiConfigStoreFs store(primary.get(), backup.get(), "fujinet.yaml");
    FujiConfig cfg = store.load();

    CHECK(cfg.general.deviceName == "backup-device");
}

TEST_CASE("YamlFujiConfigStoreFs: Load defaults when both missing")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");
    auto backup = std::make_unique<fujinet::tests::MemoryFileSystem>("backup");

    YamlFujiConfigStoreFs store(primary.get(), backup.get(), "fujinet.yaml");
    FujiConfig cfg = store.load();

    // Should return defaults (struct defaults, not YAML parsing defaults)
    CHECK(cfg.general.deviceName == ""); // empty string default
    CHECK(cfg.general.bootMode == BootMode::Config); // struct default
    CHECK(cfg.wifi.enabled == false);
    CHECK(cfg.hosts.empty());
    CHECK(cfg.mounts.empty());

    // Should have written defaults to primary
    CHECK(primary->exists("fujinet.yaml"));
    std::string content = read_file(*primary, "fujinet.yaml");
    CHECK(!content.empty());
    CHECK(content.find("fujinet:") != std::string::npos);
}

TEST_CASE("YamlFujiConfigStoreFs: Save to primary only")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");

    FujiConfig cfg{};
    cfg.general.deviceName = "saved-device";
    cfg.general.bootMode = BootMode::Cpm;
    cfg.general.altConfigFile = "/alt.yaml";
    cfg.wifi.enabled = true;
    cfg.wifi.ssid = "TestSSID";
    cfg.wifi.passphrase = "password";

    HostConfig host1{};
    host1.id = 1;
    host1.type = HostType::Sd;
    host1.name = "SD";
    host1.address = "/sd";
    host1.enabled = true;
    cfg.hosts.push_back(host1);

    MountConfig mount1{};
    mount1.id = 1;
    mount1.hostId = 1;
    mount1.path = "/disks";
    mount1.mode = "rw";
    cfg.mounts.push_back(mount1);

    cfg.modem.enabled = true;
    cfg.modem.snifferEnabled = false;
    cfg.cpm.enabled = true;
    cfg.cpm.ccpImage = "/cpm.img";
    cfg.printer.enabled = false;

    store.save(cfg);

    CHECK(primary->exists("fujinet.yaml"));
    std::string content = read_file(*primary, "fujinet.yaml");
    CHECK(!content.empty());
    CHECK(content.find("saved-device") != std::string::npos);
    CHECK(content.find("cpm") != std::string::npos);
    CHECK(content.find("TestSSID") != std::string::npos);
}

TEST_CASE("YamlFujiConfigStoreFs: Save to primary and backup")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");
    auto backup = std::make_unique<fujinet::tests::MemoryFileSystem>("backup");

    YamlFujiConfigStoreFs store(primary.get(), backup.get(), "fujinet.yaml");

    FujiConfig cfg{};
    cfg.general.deviceName = "dual-save";
    cfg.general.bootMode = BootMode::Normal;

    store.save(cfg);

    CHECK(primary->exists("fujinet.yaml"));
    CHECK(backup->exists("fujinet.yaml"));

    std::string primaryContent = read_file(*primary, "fujinet.yaml");
    std::string backupContent = read_file(*backup, "fujinet.yaml");

    CHECK(primaryContent == backupContent);
    CHECK(primaryContent.find("dual-save") != std::string::npos);
}

TEST_CASE("YamlFujiConfigStoreFs: Round-trip save and load")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");

    FujiConfig original{};
    original.general.deviceName = "roundtrip-test";
    original.general.bootMode = BootMode::Config;
    original.general.altConfigFile = "/alt.yaml";
    original.wifi.enabled = true;
    original.wifi.ssid = "RoundTripWiFi";
    original.wifi.passphrase = "secretpass";

    HostConfig h1{};
    h1.id = 1;
    h1.type = HostType::Sd;
    h1.name = "SD Card";
    h1.address = "/sd";
    h1.enabled = true;
    original.hosts.push_back(h1);

    HostConfig h2{};
    h2.id = 2;
    h2.type = HostType::Tnfs;
    h2.name = "TNFS";
    h2.address = "server.example.com";
    h2.enabled = true;
    original.hosts.push_back(h2);

    MountConfig m1{};
    m1.id = 1;
    m1.hostId = 1;
    m1.path = "/disks";
    m1.mode = "rw";
    original.mounts.push_back(m1);

    MountConfig m2{};
    m2.id = 2;
    m2.hostId = 2;
    m2.path = "/atari";
    m2.mode = "r";
    original.mounts.push_back(m2);

    original.modem.enabled = true;
    original.modem.snifferEnabled = true;
    original.cpm.enabled = true;
    original.cpm.ccpImage = "/cpm/ccp.img";
    original.printer.enabled = true;

    store.save(original);

    FujiConfig loaded = store.load();

    CHECK(configs_equal(original, loaded));
}

TEST_CASE("YamlFujiConfigStoreFs: Load empty file uses defaults")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    // Create empty file
    create_file(*primary, "/fujinet.yaml", "");

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    // Should return defaults (struct defaults, not YAML parsing defaults)
    CHECK(cfg.general.deviceName == ""); // empty string default
    CHECK(cfg.general.bootMode == BootMode::Config); // struct default
    CHECK(cfg.wifi.enabled == false);
}

TEST_CASE("YamlFujiConfigStoreFs: Load partial config with defaults")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    // Partial YAML - only some fields set
    const std::string yaml = R"(
fujinet:
  device_name: "partial-device"
wifi:
  enabled: true
  ssid: "MySSID"
hosts:
  - id: 1
    name: "SD"
    type: "SD"
    address: "/sd"
)";

    create_file(*primary, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    CHECK(cfg.general.deviceName == "partial-device");
    CHECK(cfg.general.bootMode == BootMode::Normal); // default
    CHECK(cfg.general.altConfigFile == ""); // default

    CHECK(cfg.wifi.enabled == true);
    CHECK(cfg.wifi.ssid == "MySSID");
    CHECK(cfg.wifi.passphrase == ""); // default

    REQUIRE(cfg.hosts.size() == 1);
    CHECK(cfg.hosts[0].id == 1);
    CHECK(cfg.hosts[0].name == "SD");
    CHECK(cfg.hosts[0].type == HostType::Sd);
    CHECK(cfg.hosts[0].enabled == true); // default

    CHECK(cfg.mounts.empty()); // default
    CHECK(cfg.modem.enabled == false); // default
    CHECK(cfg.cpm.enabled == false); // default
    CHECK(cfg.printer.enabled == false); // default
}

TEST_CASE("YamlFujiConfigStoreFs: Boot mode parsing")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    // Test all boot modes
    const std::string yamlNormal = R"(
fujinet:
  device_name: "test"
  boot_mode: "normal"
wifi:
  enabled: false
hosts: []
mounts: []
devices:
  modem:
    enabled: false
    sniffer_enabled: false
  cpm:
    enabled: false
    ccp_image: ""
  printer:
    enabled: false
)";

    create_file(*primary, "/fujinet.yaml", yamlNormal);
    YamlFujiConfigStoreFs store1(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg1 = store1.load();
    CHECK(cfg1.general.bootMode == BootMode::Normal);

    const std::string yamlConfig = R"(
fujinet:
  device_name: "test"
  boot_mode: "config"
wifi:
  enabled: false
hosts: []
mounts: []
devices:
  modem:
    enabled: false
    sniffer_enabled: false
  cpm:
    enabled: false
    ccp_image: ""
  printer:
    enabled: false
)";

    create_file(*primary, "/fujinet.yaml", yamlConfig);
    YamlFujiConfigStoreFs store2(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg2 = store2.load();
    CHECK(cfg2.general.bootMode == BootMode::Config);

    const std::string yamlCpm = R"(
fujinet:
  device_name: "test"
  boot_mode: "cpm"
wifi:
  enabled: false
hosts: []
mounts: []
devices:
  modem:
    enabled: false
    sniffer_enabled: false
  cpm:
    enabled: false
    ccp_image: ""
  printer:
    enabled: false
)";

    create_file(*primary, "/fujinet.yaml", yamlCpm);
    YamlFujiConfigStoreFs store3(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg3 = store3.load();
    CHECK(cfg3.general.bootMode == BootMode::Cpm);
}

TEST_CASE("YamlFujiConfigStoreFs: Host type parsing")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    const std::string yaml = R"(
fujinet:
  device_name: "test"
  boot_mode: "normal"
wifi:
  enabled: false
hosts:
  - id: 1
    name: "SD"
    type: "SD"
    address: "/sd"
  - id: 2
    name: "TNFS"
    type: "TNFS"
    address: "server.com"
  - id: 3
    name: "sd"
    type: "sd"
    address: "/sd2"
  - id: 4
    name: "tnfs"
    type: "tnfs"
    address: "server2.com"
mounts: []
devices:
  modem:
    enabled: false
    sniffer_enabled: false
  cpm:
    enabled: false
    ccp_image: ""
  printer:
    enabled: false
)";

    create_file(*primary, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    REQUIRE(cfg.hosts.size() == 4);
    CHECK(cfg.hosts[0].type == HostType::Sd);
    CHECK(cfg.hosts[1].type == HostType::Tnfs);
    CHECK(cfg.hosts[2].type == HostType::Sd); // lowercase
    CHECK(cfg.hosts[3].type == HostType::Tnfs); // lowercase
}

TEST_CASE("YamlFujiConfigStoreFs: Multiple hosts and mounts")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    const std::string yaml = R"(
fujinet:
  device_name: "multi-test"
  boot_mode: "normal"
wifi:
  enabled: false
hosts:
  - id: 1
    name: "Host1"
    type: "SD"
    address: "/sd1"
    enabled: true
  - id: 2
    name: "Host2"
    type: "TNFS"
    address: "host2.com"
    enabled: true
  - id: 3
    name: "Host3"
    type: "TNFS"
    address: "host3.com"
    enabled: false
mounts:
  - id: 1
    host_id: 1
    path: "/disks1"
    mode: "rw"
  - id: 2
    host_id: 2
    path: "/atari2"
    mode: "r"
  - id: 3
    host_id: 2
    path: "/atari3"
    mode: "rw"
devices:
  modem:
    enabled: false
    sniffer_enabled: false
  cpm:
    enabled: false
    ccp_image: ""
  printer:
    enabled: false
)";

    create_file(*primary, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    REQUIRE(cfg.hosts.size() == 3);
    REQUIRE(cfg.mounts.size() == 3);

    CHECK(cfg.mounts[0].hostId == 1);
    CHECK(cfg.mounts[1].hostId == 2);
    CHECK(cfg.mounts[2].hostId == 2); // multiple mounts on same host
}
