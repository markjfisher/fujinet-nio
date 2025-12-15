#pragma once

#include <string>

#include "fujinet/net/network_link.h"

extern "C" {
#include "esp_event.h"
#include "esp_netif.h"
}

namespace fujinet::platform::esp32 {

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

private:
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

    // avoid mutating std::string from event handler
    char _ip_buf[16]{}; // "255.255.255.255" + NUL
    bool _ip_dirty{false};
};

} // namespace fujinet::platform::esp32

