#pragma once
#include <string>

namespace fujinet::net {

enum class LinkState {
    Disconnected,
    Connecting,
    Connected,
    Failed,
};

class INetworkLink {
public:
    virtual ~INetworkLink() = default;

    virtual LinkState state() const = 0;

    virtual void connect(std::string ssid, std::string pass) = 0;
    virtual void disconnect() = 0;

    // Called from core tick (ESP32 can drive state machine here).
    virtual void poll() = 0;

    virtual std::string ip_address() const = 0;
};

}
