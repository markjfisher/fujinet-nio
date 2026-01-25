#pragma once

#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/core/io_device_manager.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fujinet::io::legacy {

// Routing-layer adapter that translates legacy network device semantics:
// - device IDs 0x71..0x78
// - commands 'O' 'C' 'R' 'W' 'S'
//
// into NetworkDevice (WireDeviceId::NetworkService / 0xFD) binary protocol commands.
//
// This keeps transports protocol-only and isolates legacy compatibility at the routing boundary.
class LegacyNetworkAdapter final : public IRequestHandler {
public:
    explicit LegacyNetworkAdapter(IODeviceManager& deviceManager);

    IOResponse handleRequest(const IORequest& request) override;

private:
    struct LegacySlot {
        std::uint16_t handle{0};
        std::uint32_t nextReadOffset{0};
        std::uint32_t nextWriteOffset{0};
        bool awaitingCommit{false}; // for POST/PUT unknown-length bodies

        // Legacy STATUS ('S') needs to expose "bytes waiting" for `network_read()`.
        // To avoid lying (or consuming bytes invisibly), we may probe the backend
        // with an offset read and cache any returned bytes here. A subsequent
        // legacy READ ('R') will drain this buffer first.
        std::vector<std::uint8_t> pendingRead{};
        bool pendingEof{false};
    };

    static constexpr DeviceID LEGACY_FIRST = 0x71;
    static constexpr DeviceID LEGACY_LAST  = 0x78;

    static bool is_legacy_net_device(DeviceID id) noexcept
    {
        return id >= LEGACY_FIRST && id <= LEGACY_LAST;
    }

    static std::size_t slot_index(DeviceID id) noexcept
    {
        return static_cast<std::size_t>(id - LEGACY_FIRST);
    }

    IODeviceManager& _deviceManager;
    std::array<LegacySlot, 8> _slots{};

    // ---- conversion helpers ----
    static std::string extract_url(const std::vector<std::uint8_t>& payload);
    static std::uint8_t method_from_aux1(std::uint8_t aux1);

    IORequest make_open_req(const IORequest& legacyReq, DeviceID legacyDeviceId);
    IORequest make_read_req(const IORequest& legacyReq, const LegacySlot& slot);
    IORequest make_write_req(const IORequest& legacyReq, const LegacySlot& slot);
    IORequest make_close_req(const IORequest& legacyReq, const LegacySlot& slot);
    IORequest make_info_req(const IORequest& legacyReq, const LegacySlot& slot);

    // Convert NetworkDevice responses back to legacy payloads.
    IOResponse convert_open_resp(const IORequest& legacyReq, const IOResponse& newResp, LegacySlot& slot);
    IOResponse convert_read_resp(const IORequest& legacyReq, const IOResponse& newResp, LegacySlot& slot);
    IOResponse convert_write_resp(const IORequest& legacyReq, const IOResponse& newResp, LegacySlot& slot);
    IOResponse convert_close_resp(const IORequest& legacyReq, const IOResponse& newResp, LegacySlot& slot);
    IOResponse convert_info_resp(const IORequest& legacyReq, const IOResponse& newResp, const LegacySlot& slot);
};

} // namespace fujinet::io::legacy

