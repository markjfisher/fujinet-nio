#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/config/fuji_config_defaults.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/io/devices/fuji_device.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using fujinet::build::BuildProfile;
using fujinet::config::BootMode;
using fujinet::config::FujiConfig;
using fujinet::config::FujiConfigStore;
using fujinet::config::UartParity;
using fujinet::config::YamlFujiConfigStoreFs;
using fujinet::config::default_config_for;
using fujinet::diag::DiagArgsView;
using fujinet::diag::DiagStatus;

namespace {

constexpr const char* kGeneric = "persist:/boot/autorun.img";
constexpr const char* kAtari = "flash:/boot/atari/autorun.atr";

BuildProfile profile_with_boot_uri(std::string_view uri)
{
    BuildProfile p{};
    p.defaultBootConfigUri = uri;
    return p;
}

// As the ESP32 packet link on UART build sets it: the link controller's rate, no pacing.
BuildProfile uart_link_profile()
{
    BuildProfile p{};
    p.defaultUartBaudRate = 2000000;
    p.defaultUartUnpaced = true;
    return p;
}

void write_file(fujinet::tests::MemoryFileSystem& fs, const std::string& path, const std::string& content)
{
    auto f = fs.open(path, "wb");
    REQUIRE(f);
    f->write(reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
}

class MemoryStore final : public FujiConfigStore {
public:
    FujiConfig load() override { return saved; }
    void save(const FujiConfig& cfg) override
    {
        saved = cfg;
        ++saves;
    }
    FujiConfig saved;
    int saves{0};
};

fujinet::diag::DiagResult run(fujinet::diag::IDiagnosticProvider& p, std::vector<std::string_view> argv)
{
    DiagArgsView args;
    args.argv = std::move(argv);
    return p.execute(args);
}

} // namespace

TEST_CASE("default_config_for: a profile without a boot URI keeps the generic default")
{
    CHECK(default_config_for(profile_with_boot_uri("")).boot.configUri == kGeneric);
}

TEST_CASE("default_config_for: a profile's boot URI replaces the generic default, nothing else")
{
    const FujiConfig cfg = default_config_for(profile_with_boot_uri(kAtari));
    CHECK(cfg.boot.configUri == kAtari);
    CHECK(cfg.boot.mode == BootMode::Config);
    CHECK(cfg.boot.readOnly == true);
}

TEST_CASE("config store: with no file the profile's boot URI is used and written for next time")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");
    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml", default_config_for(profile_with_boot_uri(kAtari)));

    CHECK(store.load().boot.configUri == kAtari);
    // The default is saved, so a later run with another profile still reads this one's.
    YamlFujiConfigStoreFs again(primary.get(), nullptr, "fujinet.yaml");
    CHECK(again.load().boot.configUri == kAtari);
}

TEST_CASE("config store: a file without a boot section, or an empty file, gets the profile's boot URI")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");
    write_file(*primary, "/fujinet.yaml", "fujinet:\n  device_name: bench\n");
    YamlFujiConfigStoreFs noBoot(primary.get(), nullptr, "fujinet.yaml", default_config_for(profile_with_boot_uri(kAtari)));
    FujiConfig cfg = noBoot.load();
    CHECK(cfg.general.deviceName == "bench");
    CHECK(cfg.boot.configUri == kAtari);

    auto empty = std::make_unique<fujinet::tests::MemoryFileSystem>("empty");
    write_file(*empty, "/fujinet.yaml", "");
    YamlFujiConfigStoreFs emptyStore(empty.get(), nullptr, "fujinet.yaml", default_config_for(profile_with_boot_uri(kAtari)));
    CHECK(emptyStore.load().boot.configUri == kAtari);
}

TEST_CASE("config store: a boot section without config_uri takes the profile's, an explicit config_uri wins")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");
    write_file(*primary, "/fujinet.yaml", "boot:\n  mode: config\n  readonly: false\n");
    YamlFujiConfigStoreFs partial(primary.get(), nullptr, "fujinet.yaml", default_config_for(profile_with_boot_uri(kAtari)));
    FujiConfig cfg = partial.load();
    CHECK(cfg.boot.configUri == kAtari);
    CHECK(cfg.boot.readOnly == false);

    auto explicitFs = std::make_unique<fujinet::tests::MemoryFileSystem>("explicit");
    write_file(*explicitFs, "/fujinet.yaml", "boot:\n  mode: config\n  config_uri: \"sd0:/mine.atr\"\n");
    YamlFujiConfigStoreFs chosen(explicitFs.get(), nullptr, "fujinet.yaml", default_config_for(profile_with_boot_uri(kAtari)));
    CHECK(chosen.load().boot.configUri == "sd0:/mine.atr");
}

TEST_CASE("default_config_for: a profile without UART defaults keeps the generic rate and pacing")
{
    const FujiConfig cfg = default_config_for(BuildProfile{});
    CHECK(cfg.channel.uart.baudRate == 115200);
    CHECK(cfg.channel.uart.txChunkSize == 16);
    CHECK(cfg.channel.uart.txChunkGapUs == 2000);
}

TEST_CASE("default_config_for: a profile's UART rate and no pacing replace the generic defaults, nothing else")
{
    const FujiConfig cfg = default_config_for(uart_link_profile());
    CHECK(cfg.channel.uart.baudRate == 2000000);
    CHECK(cfg.channel.uart.txChunkSize == 0);
    CHECK(cfg.channel.uart.txChunkGapUs == 0);
    CHECK(cfg.channel.uart.dataBits == 8);
    CHECK(cfg.channel.uart.parity == UartParity::None);
    CHECK(cfg.channel.uart.txByteGapUs == 0);
    CHECK(cfg.boot.configUri == kGeneric);
}

TEST_CASE("config store: with no file the profile's UART settings are used and written for next time")
{
    auto primary = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");
    YamlFujiConfigStoreFs store(primary.get(), nullptr, "fujinet.yaml", default_config_for(uart_link_profile()));
    CHECK(store.load().channel.uart.baudRate == 2000000);

    YamlFujiConfigStoreFs again(primary.get(), nullptr, "fujinet.yaml");
    const FujiConfig cfg = again.load();
    CHECK(cfg.channel.uart.baudRate == 2000000);
    CHECK(cfg.channel.uart.txChunkSize == 0);
    CHECK(cfg.channel.uart.txChunkGapUs == 0);
}

TEST_CASE("config store: a uart section keeps the profile's values for keys it leaves out, and its own win")
{
    auto partialFs = std::make_unique<fujinet::tests::MemoryFileSystem>("partial");
    write_file(*partialFs, "/fujinet.yaml", "channel:\n  uart:\n    parity: even\n");
    YamlFujiConfigStoreFs partial(partialFs.get(), nullptr, "fujinet.yaml", default_config_for(uart_link_profile()));
    FujiConfig cfg = partial.load();
    CHECK(cfg.channel.uart.parity == UartParity::Even);
    CHECK(cfg.channel.uart.baudRate == 2000000);
    CHECK(cfg.channel.uart.txChunkSize == 0);
    CHECK(cfg.channel.uart.txChunkGapUs == 0);

    auto explicitFs = std::make_unique<fujinet::tests::MemoryFileSystem>("explicit");
    write_file(*explicitFs, "/fujinet.yaml",
               "channel:\n  uart:\n    baud_rate: 115200\n    tx_chunk_size: 32\n");
    YamlFujiConfigStoreFs chosen(explicitFs.get(), nullptr, "fujinet.yaml", default_config_for(uart_link_profile()));
    cfg = chosen.load();
    CHECK(cfg.channel.uart.baudRate == 115200);
    CHECK(cfg.channel.uart.txChunkSize == 32);
    CHECK(cfg.channel.uart.txChunkGapUs == 0);
}

TEST_CASE("boot diagnostics exist only with a FujiDevice")
{
    CHECK(fujinet::diag::create_boot_diagnostic_provider(nullptr) == nullptr);
}

TEST_CASE("boot.get shows the boot settings and boot.* is listed")
{
    auto store = std::make_unique<MemoryStore>();
    store->saved.boot.configUri = kAtari;
    fujinet::io::FujiDevice device(nullptr, std::move(store));
    device.config_mut().boot.configUri = kAtari;
    auto diag = fujinet::diag::create_boot_diagnostic_provider(&device);
    REQUIRE(diag != nullptr);
    CHECK(diag->provider_id() == "boot");

    std::vector<fujinet::diag::DiagCommandSpec> cmds;
    diag->list_commands(cmds);
    std::vector<std::string> names;
    for (const auto& c : cmds) names.push_back(std::string(c.name));
    CHECK(names == std::vector<std::string>{"boot.get", "boot.set", "boot.save"});

    const auto r = run(*diag, {"boot.get"});
    REQUIRE(r.status == DiagStatus::Ok);
    for (const char* expected : {"mode: config", "config_uri: flash:/boot/atari/autorun.atr", "readonly: true"}) {
        CAPTURE(expected);
        CHECK(r.text.find(expected) != std::string::npos);
    }
}

TEST_CASE("boot.set changes one setting, rejects bad values, and boot.save writes the store")
{
    auto store = std::make_unique<MemoryStore>();
    MemoryStore* raw = store.get();
    fujinet::io::FujiDevice device(nullptr, std::move(store));
    auto diag = fujinet::diag::create_boot_diagnostic_provider(&device);
    REQUIRE(diag != nullptr);

    CHECK(run(*diag, {"boot.set", "config_uri", "flash:/boot/atari/autorun.atr"}).status == DiagStatus::Ok);
    CHECK(device.config().boot.configUri == "flash:/boot/atari/autorun.atr");
    CHECK(run(*diag, {"boot.set", "readonly", "false"}).status == DiagStatus::Ok);
    CHECK(device.config().boot.readOnly == false);
    CHECK(run(*diag, {"boot.set", "mode", "normal"}).status == DiagStatus::Ok);
    CHECK(device.config().boot.mode == BootMode::Normal);

    // Setting does not save: that is boot.save's job, so a typo can be abandoned.
    CHECK(raw->saves == 0);

    CHECK(run(*diag, {"boot.set", "mode", "turbo"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*diag, {"boot.set", "readonly", "maybe"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*diag, {"boot.set", "config_uri", ""}).status == DiagStatus::InvalidArgs);
    CHECK(run(*diag, {"boot.set", "colour", "red"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*diag, {"boot.set", "mode"}).status == DiagStatus::InvalidArgs);
    CHECK(device.config().boot.mode == BootMode::Normal);

    CHECK(run(*diag, {"boot.save"}).status == DiagStatus::Ok);
    CHECK(raw->saves == 1);
    CHECK(raw->saved.boot.configUri == "flash:/boot/atari/autorun.atr");
    CHECK(raw->saved.boot.readOnly == false);

    CHECK(run(*diag, {"boot.nonsense"}).status == DiagStatus::NotFound);
}

TEST_CASE("boot.save without a config store says so")
{
    fujinet::io::FujiDevice device(nullptr, nullptr);
    auto diag = fujinet::diag::create_boot_diagnostic_provider(&device);
    REQUIRE(diag != nullptr);
    CHECK(run(*diag, {"boot.save"}).status == DiagStatus::NotReady);
}
