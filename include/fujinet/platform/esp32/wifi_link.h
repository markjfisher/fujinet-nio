#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fujinet/net/network_link.h"

extern "C" {
#include "esp_event.h"
#include "esp_netif.h"
}

namespace fujinet::platform::esp32 {

struct WifiScanAp {
    std::string ssid;
    std::int8_t rssi{0};
    std::uint8_t channel{0};
    std::string auth; // human-readable, e.g. "wpa2_psk"
};

struct WifiScanResult {
    std::vector<WifiScanAp> aps;
    bool success{false};
    std::string error;
};

class Esp32WifiLink final : public fujinet::net::INetworkLink {
public:
    Esp32WifiLink();
    ~Esp32WifiLink() override;

    // sets up esp_netif, event loop, wifi init
    // NOTE: Requires NVS to be initialized by platform bootstrap.
    void init();

    fujinet::net::LinkState state() const override;

    void connect(std::string ssid, std::string pass) override;
    void disconnect() override;

    void poll() override;

    std::string ip_address() const override;

    // Blocking scan. Requires init(); starts the radio if needed.
    WifiScanResult scan();

private:
    void prepare_for_new_connection();
    bool try_wifi_connect();
    bool wait_wifi_started(int timeout_ms);
    bool wait_link_state(fujinet::net::LinkState target, int timeout_ms);
    esp_err_t start_scan_with_retries(bool allow_disconnect_for_scan, bool& disconnected_for_scan);

    static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    void on_wifi_start();
    void on_wifi_disconnected();
    void on_got_ip(const ip_event_got_ip_t* ev);

private:
    fujinet::net::LinkState _state{fujinet::net::LinkState::Disconnected};
    std::string _ssid;
    std::string _pass;
    std::string _ip;
    int _retryCount{0};
    bool _inited{false};

    esp_netif_t* _netif{nullptr};

    // event handler instances (optional for unregister in destructor)
    esp_event_handler_instance_t _wifi_handler{};
    esp_event_handler_instance_t _ip_handler{};

    bool _wifiStarted{false};
    bool _connectPending{false};
    bool _userDisconnectRequested{false};

    // avoid mutating std::string from event handler
    char _ip_buf[16]{}; // "255.255.255.255" + NUL
    bool _ip_dirty{false};
};

} // namespace fujinet::platform::esp32

