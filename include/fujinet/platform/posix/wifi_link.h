#pragma once

#include <string>

#include "fujinet/net/network_link.h"

namespace fujinet::platform::posix {

enum class WifiBackendMode {
    Unavailable,
    Host,
    Simulated,
};

class PosixWifiLink final : public net::INetworkLink {
public:
    explicit PosixWifiLink(WifiBackendMode mode, std::string interfaceName = {});

    static WifiBackendMode mode_from_environment();

    net::LinkState state() const override;
    void connect(std::string ssid, std::string pass) override;
    void set_bssid(std::string bssid) override;
    void disconnect() override;
    void poll() override;
    std::string ip_address() const override;
    net::WifiBssid current_bssid() const override;
    std::int8_t rssi() const override;
    std::string subnet_mask() const override;
    std::string gateway() const override;
    std::string dns_server() const override;
    net::WifiLinkCapabilities capabilities() const override;
    bool supports_scan() const override;
    net::WifiScanResult scan_wifi() override;

private:
    void refresh_host_state();

    WifiBackendMode _mode;
    std::string _interface;
    std::string _ssid;
    std::string _password;
    std::string _bssid_text;
    net::LinkState _state{net::LinkState::Disconnected};
    std::string _ip;
    std::string _subnet;
    std::string _gateway;
    std::string _dns;
};

} // namespace fujinet::platform::posix
