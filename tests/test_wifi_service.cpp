#include "doctest.h"

#include "fujinet/io/devices/wifi_service.h"
#include "fujinet/platform/posix/wifi_link.h"

namespace {
struct Store final : fujinet::config::FujiConfigStore {
    int saves{0};
    fujinet::config::FujiConfig load() override { return {}; }
    void save(const fujinet::config::FujiConfig&) override { ++saves; }
};

struct Link final : fujinet::net::INetworkLink {
    fujinet::net::LinkState state_value{fujinet::net::LinkState::Connected};
    bool connected{false};
    bool scan_enabled{false};
    int scans{0};
    std::string bssid;
    fujinet::net::LinkState state() const override { return state_value; }
    void connect(std::string, std::string) override { connected = true; }
    void disconnect() override { connected = false; }
    void poll() override {}
    std::string ip_address() const override { return "192.0.2.2"; }
    std::string subnet_mask() const override { return "255.255.255.0"; }
    std::string gateway() const override { return "192.0.2.1"; }
    std::string dns_server() const override { return "192.0.2.1"; }
    fujinet::net::WifiLinkCapabilities capabilities() const override {
        auto flags = static_cast<std::uint16_t>(fujinet::net::WifiCapabilityConfig |
                                            fujinet::net::WifiCapabilityStatus |
                                            fujinet::net::WifiCapabilityConnect |
                                            fujinet::net::WifiCapabilityDisconnect |
                                            fujinet::net::WifiCapabilityBssidSelect);
        if (scan_enabled) flags |= fujinet::net::WifiCapabilityScan;
        return {flags,
                fujinet::net::WifiBackendKind::Esp32};
    }
    void set_bssid(std::string value) override { bssid = std::move(value); }
    bool supports_scan() const override { return scan_enabled; }
    fujinet::net::WifiScanResult scan_wifi() override {
        ++scans;
        fujinet::net::WifiScanResult result;
        result.success = scan_enabled;
        for (const char* ssid : {"one", "two", "three"}) {
            fujinet::net::WifiScanRecord record;
            record.ssid = ssid;
            result.records.push_back(record);
        }
        return result;
    }
};
}

TEST_CASE("wifi service never returns password") {
    fujinet::config::FujiConfig config;
    config.wifi.enabled = true;
    config.wifi.ssid = "test-net";
    config.wifi.passphrase = "secret";
    Store store;
    Link link;
    fujinet::io::WifiService service(config, &store, [&] { return &link; });
    fujinet::io::IORequest request;
    request.command = 0x02;
    request.payload = {1};
    const auto response = service.handle(request);
    CHECK(response.status == fujinet::io::StatusCode::Ok);
    CHECK(std::string(response.payload.begin(), response.payload.end()).find("secret") == std::string::npos);
    CHECK(response.payload[2] == 1);
}

TEST_CASE("wifi service validates and applies write-only config") {
    fujinet::config::FujiConfig config;
    Store store;
    Link link;
    fujinet::io::WifiService service(config, &store, [&] { return &link; });
    fujinet::io::IORequest request;
    request.command = 0x03;
    request.payload = {1, 0x3f, 1, 4, 'n', 'e', 't', '1', 17,
                       'a','a',':','b','b',':','c','c',':','d','d',':','e','e',':','f','f',
                       6, 's','e','c','r','e','t'};
    const auto response = service.handle(request);
    CHECK(response.status == fujinet::io::StatusCode::Ok);
    CHECK(config.wifi.ssid == "net1");
    CHECK(config.wifi.bssid == "aa:bb:cc:dd:ee:ff");
    CHECK(config.wifi.passphrase == "secret");
    CHECK(store.saves == 1);
    CHECK(link.connected);
    CHECK(link.bssid == config.wifi.bssid);
}

TEST_CASE("wifi service rejects malformed requests") {
    fujinet::config::FujiConfig config;
    fujinet::io::WifiService service(config, nullptr, {});
    fujinet::io::IORequest request;
    request.command = 0x02;
    request.payload = {2};
    CHECK(service.handle(request).status == fujinet::io::StatusCode::InvalidRequest);
    request.payload = {1, 0};
    CHECK(service.handle(request).status == fujinet::io::StatusCode::InvalidRequest);
}

TEST_CASE("wifi service caches scans between pages") {
    fujinet::config::FujiConfig config;
    Link link;
    link.scan_enabled = true;
    fujinet::io::WifiService service(config, nullptr, [&] { return &link; });

    fujinet::io::IORequest request;
    request.command = 0x04;
    request.payload = {1, 0, 0, 2};
    auto first = service.handle(request);
    CHECK(first.status == fujinet::io::StatusCode::Ok);
    CHECK(first.payload[2] == 2);
    CHECK(first.payload[1] == 1);
    CHECK(link.scans == 1);

    request.payload = {1, 1, 0, 2};
    auto second = service.handle(request);
    CHECK(second.status == fujinet::io::StatusCode::Ok);
    CHECK(second.payload[2] == 2);
    CHECK(second.payload[1] == 0);
    CHECK(link.scans == 1);

    request.payload = {1, 0, 0, 1};
    CHECK(service.handle(request).status == fujinet::io::StatusCode::Ok);
    CHECK(link.scans == 2);
}

TEST_CASE("POSIX Wi-Fi backends advertise real capabilities") {
    using fujinet::platform::posix::PosixWifiLink;
    using fujinet::platform::posix::WifiBackendMode;

    PosixWifiLink simulated(WifiBackendMode::Simulated);
    CHECK((simulated.capabilities().flags & fujinet::net::WifiCapabilitySimulated) != 0);
    CHECK(simulated.supports_scan());
    simulated.connect("FujiNet-Sim", "secret");
    CHECK(simulated.state() == fujinet::net::LinkState::Connected);
    CHECK(simulated.scan_wifi().records.size() == 3);

    PosixWifiLink unavailable(WifiBackendMode::Unavailable);
    CHECK(!unavailable.supports_scan());
    CHECK(unavailable.capabilities().backend == fujinet::net::WifiBackendKind::Unavailable);
    unavailable.connect("ssid", "password");
    CHECK(unavailable.state() == fujinet::net::LinkState::Failed);
}
