#pragma once

#include "fujinet/io/devices/network_device.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fujinet::io {

// Friend accessor for NetworkDevice internals, used by out-of-band diagnostics providers.
// Keeps diagnostic structures and helpers out of the main device header.
struct NetworkDeviceDiagnosticsAccessor {
    struct SessionRow {
        bool active{false};
        std::uint16_t handle{0};

        std::uint8_t method{0};
        std::uint8_t flags{0};
        bool completed{false};

        bool awaitingBody{false};
        std::uint32_t expectedBodyLen{0};
        std::uint32_t receivedBodyLen{0};

        std::uint64_t createdTick{0};
        std::uint64_t lastActivityTick{0};

        std::string url;
    };

    static std::vector<SessionRow> sessions(const NetworkDevice& dev)
    {
        std::vector<SessionRow> out;
        out.reserve(NetworkDevice::MAX_SESSIONS);

        auto session_index = [&dev](const NetworkDevice::Session* s) -> std::uint8_t {
            return static_cast<std::uint8_t>(s - dev._sessions.data());
        };

        for (const auto& s : dev._sessions) {
            SessionRow row;
            row.active = s.active;
            if (s.active) {
                const std::uint8_t idx = session_index(&s);
                row.handle = NetworkDevice::make_handle(idx, s.generation);
                row.method = s.method;
                row.flags = s.flags;
                row.completed = s.completed;
                row.awaitingBody = s.awaitingBody;
                row.expectedBodyLen = s.expectedBodyLen;
                row.receivedBodyLen = s.receivedBodyLen;
                row.createdTick = s.createdTick;
                row.lastActivityTick = s.lastActivityTick;
                row.url = s.url;
            }
            out.push_back(std::move(row));
        }

        return out;
    }

    static bool close(NetworkDevice& dev, std::uint16_t handle) noexcept
    {
        const auto idx = NetworkDevice::handle_index(handle);
        const auto gen = NetworkDevice::handle_generation(handle);
        if (idx >= NetworkDevice::MAX_SESSIONS) return false;

        auto& s = dev._sessions[idx];
        if (!s.active) return false;
        if (s.generation != gen) return false;

        dev.close_and_free(s);
        return true;
    }

    static std::size_t close_all(NetworkDevice& dev) noexcept
    {
        std::size_t n = 0;
        for (auto& s : dev._sessions) {
            if (!s.active) continue;
            dev.close_and_free(s);
            ++n;
        }
        return n;
    }
};

} // namespace fujinet::io


