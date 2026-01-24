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
    // Allow out-of-band diagnostics (console) without polluting the on-wire API surface.
    friend struct NetworkDeviceDiagnosticsAccessor;

    static constexpr std::uint8_t NETPROTO_VERSION = 1;
    static constexpr std::size_t MAX_SESSIONS = 4;

    // Timeouts expressed in "device poll ticks".
    // With a 50ms tick, 20 ticks = 1s.
    static constexpr std::uint64_t IDLE_TIMEOUT_TICKS = 20ull * 60ull * 20ull; // ~20m

    struct Session {
        bool active{false};
        std::uint8_t generation{0};

        std::uint8_t method{0};
        std::uint8_t flags{0};
        std::string url;

        std::unique_ptr<INetworkProtocol> proto;

        // New: bookkeeping for reaping
        std::uint64_t createdTick{0};
        std::uint64_t lastActivityTick{0};

        // Optional: mark "completed" once response is fully readable
        // (useful when you later do async backends)
        bool completed{false};

        // HTTP request-body tracking (streamed, no large buffering in core)
        std::uint32_t expectedBodyLen = 0;   // from Open.bodyLenHint (when relevant)
        std::uint32_t receivedBodyLen = 0;   // total bytes accepted via Write()
        std::uint32_t nextBodyOffset  = 0;   // required next Write offset (sequential)
        bool          awaitingBody    = false; // gate Info/Read until body complete
        bool          bodyLenUnknown  = false; // unknown-length body; committed by zero-length Write()
    };

    std::array<Session, MAX_SESSIONS> _sessions{};
    ProtocolRegistry _registry;
    
    // local monotonic tick counter incremented from poll()
    std::uint64_t _tickNow{0};

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

    void touch(Session& s) noexcept
    {
        s.lastActivityTick = _tickNow;
    }

    void close_and_free(Session& s) noexcept
    {
        if (s.proto) {
            s.proto->close();
            s.proto.reset();
        }
        s.active = false;
        s.method = 0;
        s.flags = 0;
        s.url.clear();
        s.createdTick = 0;
        s.lastActivityTick = 0;
        s.completed = false;
    }

    // Pick a victim to evict (LRU) if we want to recover from leaky clients.
    // Returns nullptr if none active.
    Session* pick_lru_victim() noexcept
    {
        Session* victim = nullptr;
        for (auto& s : _sessions) {
            if (!s.active) continue;
            // lower lastActivityTick means it's older
            if (!victim || s.lastActivityTick < victim->lastActivityTick) {
                victim = &s;
            }
        }
        return victim;
    }

};

} // namespace fujinet::io
