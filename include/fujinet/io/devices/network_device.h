#pragma once

#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/io/devices/network_protocol_registry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fujinet::io {

// NetworkDevice: binary, chunked, handle-based protocol (v1).
// Wire device ID: WireDeviceId::NetworkService (0xFD).
// See docs/network_device_protocol.md.
class NetworkDevice : public VirtualDevice {
public:
    explicit NetworkDevice(ProtocolRegistry registry);

    IOResponse handle(const IORequest& request) override;
    void poll() override;

private:
    static constexpr std::uint8_t NETPROTO_VERSION = 1;
    static constexpr std::size_t MAX_SESSIONS = 4;

    struct Session {
        bool active{false};
        std::uint8_t generation{0};

        std::uint8_t method{0};
        std::uint8_t flags{0};
        std::string url;

        std::unique_ptr<INetworkProtocol> proto;
    };

    std::array<Session, MAX_SESSIONS> _sessions{};
    ProtocolRegistry _registry;

    static std::uint16_t make_handle(std::uint8_t idx, std::uint8_t gen) noexcept
    {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(gen) << 8) | idx);
    }

    static std::uint8_t handle_index(std::uint16_t h) noexcept
    {
        return static_cast<std::uint8_t>(h & 0xFF);
    }

    static std::uint8_t handle_generation(std::uint16_t h) noexcept
    {
        return static_cast<std::uint8_t>((h >> 8) & 0xFF);
    }
};

} // namespace fujinet::io
