#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"

#include <cstdint>
#include <string>
#include <vector>

using fujinet::config::BootMode;
using fujinet::config::FujiConfig;
using fujinet::config::MountConfig;
using fujinet::config::UartFlowControl;
using fujinet::config::UartParity;
using fujinet::config::UartStopBits;
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

    if (a.mounts.size() != b.mounts.size()) return false;
    for (std::size_t i = 0; i < a.mounts.size(); ++i) {
        if (a.mounts[i].slot != b.mounts[i].slot) return false;
        if (a.mounts[i].uri != b.mounts[i].uri) return false;
        if (a.mounts[i].mode != b.mounts[i].mode) return false;
        if (a.mounts[i].enabled != b.mounts[i].enabled) return false;
    }

    if (a.modem.enabled != b.modem.enabled) return false;
    if (a.modem.snifferEnabled != b.modem.snifferEnabled) return false;

    if (a.cpm.enabled != b.cpm.enabled) return false;
    if (a.cpm.ccpImage != b.cpm.ccpImage) return false;

    if (a.printer.enabled != b.printer.enabled) return false;
    
    if (a.netsio.enabled != b.netsio.enabled) return false;
    if (a.netsio.host != b.netsio.host) return false;
    if (a.netsio.port != b.netsio.port) return false;
    
    if (a.clock.enabled != b.clock.enabled) return false;
    if (a.clock.timezone != b.clock.timezone) return false;

    if (a.tls.trustTestCa != b.tls.trustTestCa) return false;
    
    if (a.channel.ptyPath != b.channel.ptyPath) return false;
    if (a.channel.uart.baudRate != b.channel.uart.baudRate) return false;
    if (a.channel.uart.dataBits != b.channel.uart.dataBits) return false;
    if (a.channel.uart.parity != b.channel.uart.parity) return false;
    if (a.channel.uart.stopBits != b.channel.uart.stopBits) return false;
    if (a.channel.uart.flowControl != b.channel.uart.flowControl) return false;

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
netsio:
  enabled: false
  host: "localhost"
  port: 9997
clock:
  timezone: "UTC"
  enabled: true
tls:
  trust_test_ca: false
)";

    create_file(*primary, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    CHECK(cfg.general.deviceName == "test-device");
    CHECK(cfg.general.bootMode == BootMode::Normal);
    CHECK(cfg.general.altConfigFile == "");
    CHECK(cfg.wifi.enabled == false);
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
mounts:
  - slot: 1
    uri: "sd:/disks"
    mode: "rw"
  - slot: 2
    uri: "tnfs://fujinet.online/atari"
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
netsio:
  enabled: true
  host: "netsio.example"
  port: 9998
clock:
  timezone: "Europe/London"
  enabled: true
tls:
  trust_test_ca: true
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

    REQUIRE(cfg.mounts.size() == 2);
    CHECK(cfg.mounts[0].slot == 1);
    CHECK(cfg.mounts[0].uri == "sd:/disks");
    CHECK(cfg.mounts[0].mode == "rw");

    CHECK(cfg.mounts[1].slot == 2);
    CHECK(cfg.mounts[1].uri == "tnfs://fujinet.online/atari");
    CHECK(cfg.mounts[1].mode == "r");

    CHECK(cfg.modem.enabled == true);
    CHECK(cfg.modem.snifferEnabled == true);

    CHECK(cfg.cpm.enabled == true);
    CHECK(cfg.cpm.ccpImage == "/cpm/ccp.img");

    CHECK(cfg.printer.enabled == true);
    CHECK(cfg.netsio.enabled == true);
    CHECK(cfg.netsio.host == "netsio.example");
    CHECK(cfg.netsio.port == 9998);
    CHECK(cfg.clock.timezone == "Europe/London");
    CHECK(cfg.tls.trustTestCa == true);
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
netsio:
  enabled: false
  host: "localhost"
  port: 9997
clock:
  timezone: "UTC"
  enabled: true
tls:
  trust_test_ca: false
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
netsio:
  enabled: false
  host: "localhost"
  port: 9997
clock:
  timezone: "UTC"
  enabled: true
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

    MountConfig mount1{};
    mount1.slot = 1;
    mount1.uri = "sd:/disks";
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

    MountConfig m1{};
    m1.slot = 1;
    m1.uri = "sd:/disks";
    m1.mode = "rw";
    original.mounts.push_back(m1);

    MountConfig m2{};
    m2.slot = 2;
    m2.uri = "tnfs://server.example.com/atari";
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
netsio:
  enabled: false
  host: "localhost"
  port: 9997
clock:
  timezone: "UTC"
  enabled: true
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

TEST_CASE("YamlFujiConfigStoreFs: Multiple mounts")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    const std::string yaml = R"(
fujinet:
  device_name: "multi-test"
  boot_mode: "normal"
wifi:
  enabled: false
mounts:
  - id: 1
    uri: "sd:/disks1"
    mode: "rw"
  - id: 2
    uri: "tnfs://host2.com/atari2"
    mode: "r"
  - id: 3
    uri: "tnfs://host2.com/atari3"
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

    REQUIRE(cfg.mounts.size() == 3);

    // Note: 'id' is no longer read from YAML, so slot will be 0 (unassigned)
    // This test documents that 'id' in YAML is NOT supported - use 'slot' instead
    CHECK(cfg.mounts[0].slot == 0);
    CHECK(cfg.mounts[0].uri == "sd:/disks1");
    CHECK(cfg.mounts[0].mode == "rw");

    CHECK(cfg.mounts[1].slot == 0);
    CHECK(cfg.mounts[1].uri == "tnfs://host2.com/atari2");
    CHECK(cfg.mounts[1].mode == "r");

    CHECK(cfg.mounts[2].slot == 0);
    CHECK(cfg.mounts[2].uri == "tnfs://host2.com/atari3");
    CHECK(cfg.mounts[2].mode == "rw");
}

TEST_CASE("YamlFujiConfigStoreFs: Load config with new slot field")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    // YAML config using new 'slot' field instead of legacy 'id'
    const std::string yaml = R"(
fujinet:
  device_name: "slot-test"
  boot_mode: "normal"
wifi:
  enabled: false
mounts:
  - slot: 1
    uri: "sd:/disks/boot.atr"
    mode: "rw"
    enabled: true
  - slot: 2
    uri: "tnfs://192.168.1.100:16384/atari/games.atr"
    mode: "r"
    enabled: true
  - slot: 3
    uri: "host:/images/data.atr"
    mode: "rw"
    enabled: false
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

    REQUIRE(cfg.mounts.size() == 3);

    // Check slot field
    CHECK(cfg.mounts[0].slot == 1);
    CHECK(cfg.mounts[0].uri == "sd:/disks/boot.atr");
    CHECK(cfg.mounts[0].mode == "rw");
    CHECK(cfg.mounts[0].enabled == true);

    CHECK(cfg.mounts[1].slot == 2);
    CHECK(cfg.mounts[1].uri == "tnfs://192.168.1.100:16384/atari/games.atr");
    CHECK(cfg.mounts[1].mode == "r");
    CHECK(cfg.mounts[1].enabled == true);

    CHECK(cfg.mounts[2].slot == 3);
    CHECK(cfg.mounts[2].uri == "host:/images/data.atr");
    CHECK(cfg.mounts[2].mode == "rw");
    CHECK(cfg.mounts[2].enabled == false);
}

TEST_CASE("MountConfig: effective_slot")
{
    using fujinet::config::MountConfig;

    // Test explicit slot
    MountConfig m1;
    m1.slot = 3;
    CHECK(m1.effective_slot() == 2);  // slot 3 -> index 2

    // Test slot 1
    MountConfig m2;
    m2.slot = 1;
    CHECK(m2.effective_slot() == 0);  // slot 1 -> index 0

    // Test slot 8
    MountConfig m3;
    m3.slot = 8;
    CHECK(m3.effective_slot() == 7);  // slot 8 -> index 7

    // Test unassigned
    MountConfig m4;
    m4.slot = 0;
    CHECK(m4.effective_slot() == -1);  // Unassigned
}

TEST_CASE("YamlFujiConfigStoreFs: Channel ptyPath config")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    // YAML config with ptyPath set
    const std::string yaml = R"(
fujinet:
  device_name: "test-device"
  boot_mode: "normal"
  alt_config_file: ""
wifi:
  enabled: false
  ssid: ""
  passphrase: ""
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
channel:
  pty_path: "/dev/fujinet-pty"
)";

    create_file(*primary, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    CHECK(cfg.general.deviceName == "test-device");
    CHECK(cfg.general.bootMode == BootMode::Normal);
    CHECK(cfg.general.altConfigFile == "");
    CHECK(cfg.wifi.enabled == false);
    CHECK(cfg.mounts.empty());
    CHECK(cfg.channel.ptyPath == "/dev/fujinet-pty");
}

TEST_CASE("YamlFujiConfigStoreFs: Channel uart nested map")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    const std::string yaml = R"(
fujinet:
  device_name: "test-device"
  boot_mode: "normal"
  alt_config_file: ""
wifi:
  enabled: false
  ssid: ""
  passphrase: ""
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
channel:
  uart:
    baud_rate: 9600
    data_bits: 7
    parity: even
    stop_bits: "2"
    flow_control: rts_cts
)";

    create_file(*primary, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    CHECK(cfg.channel.uart.baudRate == 9600u);
    CHECK(cfg.channel.uart.dataBits == 7);
    CHECK(cfg.channel.uart.parity == UartParity::Even);
    CHECK(cfg.channel.uart.stopBits == UartStopBits::Two);
    CHECK(cfg.channel.uart.flowControl == UartFlowControl::RtsCts);
}

TEST_CASE("YamlFujiConfigStoreFs: Channel legacy uart_baud key")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    const std::string yaml = R"(
fujinet:
  device_name: "test-device"
  boot_mode: "normal"
  alt_config_file: ""
wifi:
  enabled: false
  ssid: ""
  passphrase: ""
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
channel:
  uart_baud: 9600
)";

    create_file(*primary, "/fujinet.yaml", yaml);

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    FujiConfig cfg = store.load();

    CHECK(cfg.channel.uart.baudRate == 9600u);
    CHECK(cfg.channel.uart.dataBits == 8);
    CHECK(cfg.channel.uart.parity == UartParity::None);
}

TEST_CASE("YamlFujiConfigStoreFs: Channel ptyPath empty default")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    // YAML config without ptyPath (should default to empty)
    const std::string yaml = R"(
fujinet:
  device_name: "test-device"
  boot_mode: "normal"
  alt_config_file: ""
wifi:
  enabled: false
  ssid: ""
  passphrase: ""
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
    CHECK(cfg.mounts.empty());
    CHECK(cfg.channel.ptyPath.empty());  // Should be empty by default
}

TEST_CASE("YamlFujiConfigStoreFs: Round-trip save and load with ptyPath")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");

    FujiConfig original{};
    original.general.deviceName = "roundtrip-test";
    original.general.bootMode = BootMode::Config;
    original.general.altConfigFile = "/alt.yaml";
    original.wifi.enabled = true;
    original.wifi.ssid = "RoundTripWiFi";
    original.wifi.passphrase = "secretpass";

    MountConfig m1{};
    m1.slot = 1;
    m1.uri = "sd:/disks";
    m1.mode = "rw";
    original.mounts.push_back(m1);

    MountConfig m2{};
    m2.slot = 2;
    m2.uri = "tnfs://server.example.com/atari";
    m2.mode = "r";
    original.mounts.push_back(m2);

    original.modem.enabled = true;
    original.modem.snifferEnabled = true;
    original.cpm.enabled = true;
    original.cpm.ccpImage = "/cpm/ccp.img";
    original.printer.enabled = true;
    
    // Set ptyPath
    original.channel.ptyPath = "/dev/fujinet-pty";

    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml");
    store.save(original);

    FujiConfig loaded = store.load();

    CHECK(configs_equal(original, loaded));
}
