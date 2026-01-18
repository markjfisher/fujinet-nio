#pragma once

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/net_commands.h"

#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>

namespace fujinet::io::transport::legacy {

// Internal helper for legacy transports to bridge legacy network device IDs (0x71-0x78)
// to NetworkService (0xFD). This is transport-internal and never exposed to core services.
class LegacyNetworkBridge {
public:
    LegacyNetworkBridge();
    ~LegacyNetworkBridge() = default;

    // Check if device ID is a legacy network device (0x71-0x78)
    static bool isLegacyNetworkDevice(DeviceID deviceId);

    // Convert legacy network request to NetworkService format
    // Returns true if conversion was performed, false if not a legacy network device
    bool convertRequest(IORequest& req);

    // Convert NetworkService response back to legacy format
    // Returns true if conversion was performed, false if request wasn't converted
    // Looks up the original request by response ID
    bool convertResponse(IOResponse& resp);

private:
    // Info stored for each converted request (for response conversion)
    struct LegacyRequestInfo {
        DeviceID deviceId;
        std::uint16_t command;
    };
    // Convert legacy OPEN command to NetworkService Open
    IORequest convertOpen(const IORequest& legacyReq);

    // Convert legacy READ command to NetworkService Read
    IORequest convertRead(const IORequest& legacyReq);

    // Convert legacy WRITE command to NetworkService Write
    IORequest convertWrite(const IORequest& legacyReq);

    // Convert legacy CLOSE command to NetworkService Close
    IORequest convertClose(const IORequest& legacyReq);

    // Convert legacy STATUS command to NetworkService Info
    IORequest convertStatus(const IORequest& legacyReq);

    // Convert NetworkService response back to legacy format
    IOResponse convertResponseInternal(const IORequest& legacyReq, const IOResponse& newResp);

    // Get handle for legacy device ID, or 0 if not found
    std::uint16_t getHandle(DeviceID legacyDeviceId) const;

    // Store handle for legacy device ID
    void setHandle(DeviceID legacyDeviceId, std::uint16_t handle);

    // Clear handle for legacy device ID
    void clearHandle(DeviceID legacyDeviceId);

    // Extract URL from legacy payload (remove "N:" prefix if present)
    std::string extractUrl(const std::vector<std::uint8_t>& payload);

    // Convert legacy mode (aux1) to HTTP method
    std::uint8_t convertMode(std::uint8_t aux1);

    // Map legacy device ID (0x71-0x78) → NetworkService handle
    std::unordered_map<DeviceID, std::uint16_t> _handleMap;
    
    // Map request ID → legacy request info (for response conversion)
    std::unordered_map<RequestID, LegacyRequestInfo> _requestToLegacyInfo;

    static constexpr DeviceID NETWORK_SERVICE_ID = 0xFD;
    static constexpr DeviceID LEGACY_NETWORK_FIRST = 0x71;
    static constexpr DeviceID LEGACY_NETWORK_LAST = 0x78;
    
    // Legacy command codes
    static constexpr std::uint8_t CMD_OPEN = 'O';   // 0x4F
    static constexpr std::uint8_t CMD_CLOSE = 'C';  // 0x43
    static constexpr std::uint8_t CMD_READ = 'R';   // 0x52
    static constexpr std::uint8_t CMD_WRITE = 'W';  // 0x57
    static constexpr std::uint8_t CMD_STATUS = 'S'; // 0x53
};

} // namespace fujinet::io::transport::legacy
