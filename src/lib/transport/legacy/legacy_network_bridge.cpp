#include "fujinet/io/transport/legacy/legacy_network_bridge.h"
#include "fujinet/io/devices/net_codec.h"
#include "fujinet/core/logging.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "legacy_net_bridge";

LegacyNetworkBridge::LegacyNetworkBridge()
{
    FN_LOGI(TAG, "LegacyNetworkBridge created");
}

bool LegacyNetworkBridge::isLegacyNetworkDevice(DeviceID deviceId)
{
    return deviceId >= LEGACY_NETWORK_FIRST && deviceId <= LEGACY_NETWORK_LAST;
}

bool LegacyNetworkBridge::convertRequest(IORequest& req)
{
    if (!isLegacyNetworkDevice(req.deviceId)) {
        return false; // Not a legacy network device, no conversion needed
    }

    FN_LOGI(TAG, "Converting legacy network request: device=0x%02X, command=0x%02X ('%c')",
            req.deviceId, req.command, 
            (req.command >= 32 && req.command < 127) ? static_cast<char>(req.command) : '?');

    // Store mapping for response conversion
    DeviceID legacyDeviceId = req.deviceId;

    // Convert legacy command to new protocol
    IORequest newReq;
    switch (req.command) {
    case CMD_OPEN:
        newReq = convertOpen(req);
        break;
    case CMD_READ:
        newReq = convertRead(req);
        break;
    case CMD_WRITE:
        newReq = convertWrite(req);
        break;
    case CMD_CLOSE:
        newReq = convertClose(req);
        break;
    case CMD_STATUS:
        newReq = convertStatus(req);
        break;
    default:
        FN_LOGW(TAG, "Unknown legacy network command: 0x%02X", req.command);
        return false; // Don't convert, let it fail normally
    }

    // Store mapping: request ID → (legacy device ID, command) for response conversion
    LegacyNetworkBridge::LegacyRequestInfo info;
    info.deviceId = legacyDeviceId;
    info.command = req.command;
    _requestToLegacyInfo[newReq.id] = info;

    // Replace request with converted version
    req = newReq;
    return true;
}

bool LegacyNetworkBridge::convertResponse(IOResponse& resp)
{
    // Check if this response corresponds to a converted legacy network request
    auto it = _requestToLegacyInfo.find(resp.id);
    if (it == _requestToLegacyInfo.end()) {
        return false; // Not a converted request, no conversion needed
    }

    const LegacyNetworkBridge::LegacyRequestInfo& info = it->second;
    
    // Reconstruct minimal legacy request for conversion
    IORequest legacyReq;
    legacyReq.id = resp.id;
    legacyReq.deviceId = info.deviceId;
    legacyReq.command = info.command;

    // Convert response back to legacy format
    IOResponse legacyResp = convertResponseInternal(legacyReq, resp);
    
    // Replace response with converted version
    resp = legacyResp;
    
    // Clean up mapping if this was a CLOSE command
    if (info.command == CMD_CLOSE) {
        _requestToLegacyInfo.erase(it);
    }

    return true;
}

IORequest LegacyNetworkBridge::convertOpen(const IORequest& legacyReq)
{
    FN_LOGI(TAG, "Converting OPEN: device=0x%02X, aux1=0x%02X, aux2=0x%02X, payload_size=%zu",
            legacyReq.deviceId, 
            legacyReq.params.size() > 0 ? legacyReq.params[0] : 0,
            legacyReq.params.size() > 1 ? legacyReq.params[1] : 0,
            legacyReq.payload.size());

    // Extract URL from payload
    std::string url = extractUrl(legacyReq.payload);
    if (url.empty()) {
        FN_LOGW(TAG, "OPEN: Empty URL in payload");
    }

    // Convert aux1/aux2 to mode/flags
    std::uint8_t aux1 = legacyReq.params.size() > 0 ? static_cast<std::uint8_t>(legacyReq.params[0]) : 12;
    std::uint8_t aux2 = legacyReq.params.size() > 1 ? static_cast<std::uint8_t>(legacyReq.params[1]) : 0;
    
    std::uint8_t method = convertMode(aux1);
    std::uint8_t flags = aux2 & 0x07; // Lower 3 bits for TLS/follow_redirects

    // Build binary protocol payload
    std::string payload;
    payload.reserve(1 + 1 + 1 + 2 + url.size() + 2 + 4 + 2);

    // Version
    netproto::write_u8(payload, 1);
    
    // Method
    netproto::write_u8(payload, method);
    
    // Flags
    netproto::write_u8(payload, flags);
    
    // URL (length-prefixed)
    netproto::write_lp_u16_string(payload, url);
    
    // Header count (0 for legacy)
    netproto::write_u16le(payload, 0);
    
    // Body length hint
    // For POST/PUT (aux1=8, 13, or 14), set a large hint so HTTP protocol defers until WRITE completes
    // For GET/DELETE/other methods, use 0 (no body)
    // TODO: Legacy POST/PUT doesn't know body length upfront. We use a large value (1MB) as a workaround.
    // The HTTP protocol will defer until write_body() accumulates data, but it won't auto-dispatch
    // because it checks for exact match. We need to handle this properly (dispatch on READ or zero-length WRITE).
    std::uint32_t bodyLenHint = 0;
    if (method == 2 || method == 3) { // POST or PUT
        // Use 1MB as a reasonable maximum for legacy POST/PUT body
        // This allows WRITE commands to accumulate, but the request won't auto-dispatch
        // until we implement proper "body complete" detection (e.g., on READ or zero-length WRITE)
        bodyLenHint = 1024 * 1024; // 1MB
    }
    netproto::write_u32le(payload, bodyLenHint);
    
    // Response header count (0 for legacy)
    netproto::write_u16le(payload, 0);

    IORequest newReq;
    newReq.id = legacyReq.id;
    newReq.deviceId = NETWORK_SERVICE_ID;
    newReq.type = RequestType::Command;
    newReq.command = static_cast<std::uint16_t>(protocol::NetworkCommand::Open);
    newReq.payload.assign(payload.begin(), payload.end());

    FN_LOGI(TAG, "Converted OPEN: url='%s', method=%u, flags=0x%02X", url.c_str(), method, flags);

    return newReq;
}

IORequest LegacyNetworkBridge::convertRead(const IORequest& legacyReq)
{
    // Get handle for this legacy device ID
    std::uint16_t handle = getHandle(legacyReq.deviceId);
    if (handle == 0) {
        FN_LOGW(TAG, "READ: No handle for device 0x%02X", legacyReq.deviceId);
    }

    // aux1/aux2 = byte count (little-endian 16-bit)
    std::uint16_t byteCount = 0;
    if (legacyReq.params.size() >= 2) {
        byteCount = static_cast<std::uint16_t>(
            (legacyReq.params[0] & 0xFF) | ((legacyReq.params[1] & 0xFF) << 8)
        );
    } else if (legacyReq.params.size() == 1) {
        byteCount = static_cast<std::uint16_t>(legacyReq.params[0] & 0xFF);
    } else {
        byteCount = 256; // Default legacy read size
    }

    // Build binary protocol payload
    std::string payload;
    payload.reserve(1 + 2 + 4 + 2);

    // Version
    netproto::write_u8(payload, 1);
    
    // Handle
    netproto::write_u16le(payload, handle);
    
    // Offset (0 for legacy)
    netproto::write_u32le(payload, 0);
    
    // Max bytes
    netproto::write_u16le(payload, byteCount);

    IORequest newReq;
    newReq.id = legacyReq.id;
    newReq.deviceId = NETWORK_SERVICE_ID;
    newReq.type = RequestType::Command;
    newReq.command = static_cast<std::uint16_t>(protocol::NetworkCommand::Read);
    newReq.payload.assign(payload.begin(), payload.end());

    FN_LOGI(TAG, "Converted READ: handle=%u, byteCount=%u", handle, byteCount);

    return newReq;
}

IORequest LegacyNetworkBridge::convertWrite(const IORequest& legacyReq)
{
    // Get handle for this legacy device ID
    std::uint16_t handle = getHandle(legacyReq.deviceId);
    if (handle == 0) {
        FN_LOGW(TAG, "WRITE: No handle for device 0x%02X", legacyReq.deviceId);
    }

    // Build binary protocol payload
    std::string payload;
    payload.reserve(1 + 2 + 4 + 2 + legacyReq.payload.size());

    // Version
    netproto::write_u8(payload, 1);
    
    // Handle
    netproto::write_u16le(payload, handle);
    
    // Offset (0 for legacy)
    netproto::write_u32le(payload, 0);
    
    // Data length
    netproto::write_u16le(payload, static_cast<std::uint16_t>(legacyReq.payload.size()));
    
    // Data
    netproto::write_bytes(payload, legacyReq.payload.data(), legacyReq.payload.size());

    IORequest newReq;
    newReq.id = legacyReq.id;
    newReq.deviceId = NETWORK_SERVICE_ID;
    newReq.type = RequestType::Command;
    newReq.command = static_cast<std::uint16_t>(protocol::NetworkCommand::Write);
    newReq.payload.assign(payload.begin(), payload.end());

    FN_LOGI(TAG, "Converted WRITE: handle=%u, dataLen=%zu", handle, legacyReq.payload.size());

    return newReq;
}

IORequest LegacyNetworkBridge::convertClose(const IORequest& legacyReq)
{
    // Get handle for this legacy device ID
    std::uint16_t handle = getHandle(legacyReq.deviceId);
    if (handle == 0) {
        FN_LOGW(TAG, "CLOSE: No handle for device 0x%02X", legacyReq.deviceId);
    }

    // Build binary protocol payload
    std::string payload;
    payload.reserve(1 + 2);

    // Version
    netproto::write_u8(payload, 1);
    
    // Handle
    netproto::write_u16le(payload, handle);

    IORequest newReq;
    newReq.id = legacyReq.id;
    newReq.deviceId = NETWORK_SERVICE_ID;
    newReq.type = RequestType::Command;
    newReq.command = static_cast<std::uint16_t>(protocol::NetworkCommand::Close);
    newReq.payload.assign(payload.begin(), payload.end());

    FN_LOGI(TAG, "Converted CLOSE: handle=%u", handle);

    return newReq;
}

IORequest LegacyNetworkBridge::convertStatus(const IORequest& legacyReq)
{
    // Get handle for this legacy device ID
    std::uint16_t handle = getHandle(legacyReq.deviceId);
    if (handle == 0) {
        FN_LOGW(TAG, "STATUS: No handle for device 0x%02X", legacyReq.deviceId);
    }

    // Build binary protocol payload
    std::string payload;
    payload.reserve(1 + 2);

    // Version
    netproto::write_u8(payload, 1);
    
    // Handle
    netproto::write_u16le(payload, handle);

    IORequest newReq;
    newReq.id = legacyReq.id;
    newReq.deviceId = NETWORK_SERVICE_ID;
    newReq.type = RequestType::Command;
    newReq.command = static_cast<std::uint16_t>(protocol::NetworkCommand::Info);
    newReq.payload.assign(payload.begin(), payload.end());

    FN_LOGI(TAG, "Converted STATUS: handle=%u", handle);

    return newReq;
}

IOResponse LegacyNetworkBridge::convertResponseInternal(const IORequest& legacyReq, const IOResponse& newResp)
{
    IOResponse legacyResp;
    legacyResp.id = legacyReq.id;
    legacyResp.deviceId = legacyReq.deviceId;
    legacyResp.command = legacyReq.command;
    legacyResp.status = newResp.status;

    // Handle command-specific response conversion
    switch (legacyReq.command) {
    case CMD_OPEN: {
        if (newResp.status == StatusCode::Ok && newResp.payload.size() >= 6) {
            // Extract handle from response
            // Response format: version(1) + flags(1) + reserved(2) + handle(2)
            std::uint16_t handle = static_cast<std::uint16_t>(
                newResp.payload[4] | (newResp.payload[5] << 8)
            );
            setHandle(legacyReq.deviceId, handle);
            FN_LOGI(TAG, "OPEN successful: device=0x%02X → handle=%u", legacyReq.deviceId, handle);
            // Legacy clients don't see handles, so return empty payload
            legacyResp.payload.clear();
        } else {
            FN_LOGW(TAG, "OPEN failed: status=%d", static_cast<int>(newResp.status));
        }
        break;
    }

    case CMD_READ: {
        if (newResp.status == StatusCode::Ok) {
            // Extract data from response
            // Response format: version(1) + flags(1) + reserved(2) + handle(2) + offset(4) + dataLen(2) + data
            // Total header = 4 (common prefix) + 2 (handle) + 4 (offset) + 2 (dataLen) = 12 bytes
            if (newResp.payload.size() >= 12) {
                std::uint16_t dataLen = static_cast<std::uint16_t>(
                    newResp.payload[10] | (newResp.payload[11] << 8)
                );
                if (newResp.payload.size() >= 12 + dataLen) {
                    legacyResp.payload.assign(
                        newResp.payload.begin() + 12,
                        newResp.payload.begin() + 12 + dataLen
                    );
                    FN_LOGI(TAG, "READ: Extracted %u bytes from response (total payload size=%zu)",
                            dataLen, newResp.payload.size());
                } else {
                    FN_LOGW(TAG, "READ: Response payload too small: expected %u bytes, got %zu",
                            12 + dataLen, newResp.payload.size());
                }
            } else {
                FN_LOGW(TAG, "READ: Response payload too small: expected at least 12 bytes, got %zu",
                        newResp.payload.size());
            }
        } else {
            FN_LOGW(TAG, "READ: Response has error status=%d", static_cast<int>(newResp.status));
        }
        break;
    }

    case CMD_WRITE: {
        // Legacy WRITE returns empty payload on success
        legacyResp.payload.clear();
        break;
    }

    case CMD_CLOSE: {
        // Clear handle mapping
        clearHandle(legacyReq.deviceId);
        // Legacy CLOSE returns empty payload on success
        legacyResp.payload.clear();
        break;
    }

    case CMD_STATUS: {
        // If STATUS failed with InvalidRequest (no handle), create synthetic "not connected" response
        if (newResp.status == StatusCode::InvalidRequest) {
            FN_LOGI(TAG, "STATUS: InvalidRequest (no handle), creating synthetic 'not connected' response");
            legacyResp.status = StatusCode::Ok; // Return Ok with "not connected" data
            // Legacy status format: 4 bytes [bytesWaitingLow, bytesWaitingHigh, connected, error]
            // bytesWaiting=0, connected=0, error=136 (EOF/not connected)
            legacyResp.payload = {0x00, 0x00, 0x00, 136};
        } else if (newResp.status == StatusCode::NotReady) {
            // Request not ready yet (e.g., HTTP request not dispatched, or body still uploading)
            // Return "not ready" status: 0 bytes waiting, connected=0, error=136 (EOF/not ready)
            FN_LOGI(TAG, "STATUS: NotReady, returning 'not ready' response");
            legacyResp.status = StatusCode::Ok;
            legacyResp.payload = {0x00, 0x00, 0x00, 136};
        } else if (newResp.status == StatusCode::Ok) {
            // Convert Info response to legacy status format
            // Info response format: version(1) + flags(1) + reserved(2) + handle(2) + httpStatus(2) + contentLength(8) + headerLen(2) + headers
            // Legacy status format: 4 bytes [bytesWaitingLow, bytesWaitingHigh, connected, error]
            // connected: 1 = transaction in progress (still receiving), 0 = transaction complete
            // error: 1 = success, >1 = error code
            if (newResp.payload.size() >= 18) {
                // Extract httpStatus (at offset 6-7, after version(1) + flags(1) + reserved(2) + handle(2))
                std::uint16_t httpStatus = static_cast<std::uint16_t>(newResp.payload[6]) |
                                          (static_cast<std::uint16_t>(newResp.payload[7]) << 8);
                
                // Extract contentLength (bytes waiting) from Info response (at offset 8-15)
                std::uint64_t contentLength = static_cast<std::uint64_t>(newResp.payload[8]) |
                                             (static_cast<std::uint64_t>(newResp.payload[9]) << 8) |
                                             (static_cast<std::uint64_t>(newResp.payload[10]) << 16) |
                                             (static_cast<std::uint64_t>(newResp.payload[11]) << 24) |
                                             (static_cast<std::uint64_t>(newResp.payload[12]) << 32) |
                                             (static_cast<std::uint64_t>(newResp.payload[13]) << 40) |
                                             (static_cast<std::uint64_t>(newResp.payload[14]) << 48) |
                                             (static_cast<std::uint64_t>(newResp.payload[15]) << 56);
                
                // Clamp contentLength to 16-bit for legacy (max 65535)
                std::uint16_t bytesWaiting = (contentLength > 65535) ? 65535 : static_cast<std::uint16_t>(contentLength);
                
                // For GET requests, connected=1 means transaction in progress (still receiving data)
                // For now, assume connected=1 if we have data available (matching old firmware behavior)
                // In old firmware, connected=1 means is_transaction_done()==false (still receiving)
                // We'll set connected=1 if contentLength > 0 (data available to read)
                std::uint8_t connected = (contentLength > 0) ? 1 : 0;
                
                // error: 1 = success (data available or transaction in progress), 136 = EOF (normal, all data read)
                // Legacy error codes: 1=OK, 136=END_OF_FILE (normal when done), others are HTTP-specific
                std::uint8_t error = 1; // Default to success
                if (httpStatus == 0) {
                    // No HTTP status yet (shouldn't happen if performed, but be defensive)
                    error = 136; // EOF (not ready)
                } else if (httpStatus >= 200 && httpStatus < 300) {
                    // HTTP success: error=1 if data available, error=136 if EOF (all data read)
                    if (contentLength == 0) {
                        error = 136; // EOF - all data has been read
                    } else {
                        error = 1; // Success - data available
                    }
                } else if (httpStatus == 404 || httpStatus == 410) {
                    error = 170; // FILE_NOT_FOUND
                } else if (httpStatus == 401 || httpStatus == 403) {
                    error = 165; // INVALID_USERNAME_OR_PASSWORD
                } else if (httpStatus >= 400 && httpStatus < 500) {
                    error = 144; // CLIENT_GENERAL
                } else if (httpStatus >= 500) {
                    error = 146; // SERVER_GENERAL
                } else {
                    error = 136; // EOF (for other cases)
                }
                
                legacyResp.payload = {
                    static_cast<std::uint8_t>(bytesWaiting & 0xFF),
                    static_cast<std::uint8_t>((bytesWaiting >> 8) & 0xFF),
                    connected,
                    error
                };
                
                FN_LOGI(TAG, "STATUS: httpStatus=%u, contentLength=%llu, bytesWaiting=%u, connected=%u, error=%u",
                        httpStatus, static_cast<unsigned long long>(contentLength), bytesWaiting, connected, error);
            } else {
                // Fallback: malformed Info response
                FN_LOGW(TAG, "STATUS: Info response too small (%zu bytes), returning fallback", newResp.payload.size());
                legacyResp.payload = {0x00, 0x00, 0x00, 136}; // EOF
            }
        } else {
            // Other error - return error status
            FN_LOGW(TAG, "STATUS: Error status=%u, returning error response", static_cast<unsigned>(newResp.status));
            legacyResp.payload = {0x00, 0x00, 0x00, 136}; // EOF
        }
        break;
    }

    default:
        legacyResp.payload = newResp.payload;
        break;
    }

    return legacyResp;
}

std::uint16_t LegacyNetworkBridge::getHandle(DeviceID legacyDeviceId) const
{
    auto it = _handleMap.find(legacyDeviceId);
    return (it != _handleMap.end()) ? it->second : 0;
}

void LegacyNetworkBridge::setHandle(DeviceID legacyDeviceId, std::uint16_t handle)
{
    _handleMap[legacyDeviceId] = handle;
    FN_LOGI(TAG, "Stored handle: device=0x%02X → handle=%u", legacyDeviceId, handle);
}

void LegacyNetworkBridge::clearHandle(DeviceID legacyDeviceId)
{
    auto it = _handleMap.find(legacyDeviceId);
    if (it != _handleMap.end()) {
        FN_LOGI(TAG, "Cleared handle: device=0x%02X (was handle=%u)", legacyDeviceId, it->second);
        _handleMap.erase(it);
    }
}

std::string LegacyNetworkBridge::extractUrl(const std::vector<std::uint8_t>& payload)
{
    if (payload.empty()) {
        return "";
    }

    // Convert payload to string (legacy uses ASCII/PETSCII)
    std::string url;
    url.reserve(payload.size());
    for (std::uint8_t b : payload) {
        // Skip null bytes
        if (b == 0) {
            break;
        }
        url.push_back(static_cast<char>(b));
    }

    // Remove "N:" or "n:" prefix if present (legacy device spec format)
    if (url.size() >= 2 && url[1] == ':' && (url[0] == 'N' || url[0] == 'n')) {
        url = url.substr(2);
    }

    return url;
}

std::uint8_t LegacyNetworkBridge::convertMode(std::uint8_t aux1)
{
    // Legacy aux1 values (from Protocol.h):
    // 4 = PROTOCOL_OPEN_READ (GET, with filename translation, URL encoding)
    // 5 = PROTOCOL_OPEN_HTTP_DELETE (DELETE, no headers)
    // 6 = PROTOCOL_OPEN_DIRECTORY (PROPFIND, WebDAV directory) - not handled here
    // 8 = PROTOCOL_OPEN_WRITE (PUT, write data to server)
    // 9 = PROTOCOL_OPEN_APPEND (DELETE, with headers)
    // 12 = PROTOCOL_OPEN_READWRITE (GET, pure and unmolested)
    // 13 = PROTOCOL_OPEN_HTTP_POST (POST, write sends post data to server)
    // 14 = PROTOCOL_OPEN_HTTP_PUT (PUT, write sends post data to server)
    // 
    // New protocol methods:
    // 1 = GET
    // 2 = POST
    // 3 = PUT
    // 4 = DELETE
    // 5 = HEAD

    switch (aux1) {
    case 4:  // PROTOCOL_OPEN_READ
        return 1; // GET
    case 5:  // PROTOCOL_OPEN_HTTP_DELETE
        return 4; // DELETE
    case 8:  // PROTOCOL_OPEN_WRITE
        return 3; // PUT
    case 9:  // PROTOCOL_OPEN_APPEND
        return 4; // DELETE
    case 12: // PROTOCOL_OPEN_READWRITE
        return 1; // GET (read/write, default to GET)
    case 13: // PROTOCOL_OPEN_HTTP_POST
        return 2; // POST (write sends post data)
    case 14: // PROTOCOL_OPEN_HTTP_PUT
        return 3; // PUT (write sends post data)
    default:
        // Default to GET for unknown modes
        FN_LOGW(TAG, "Unknown aux1 value: %u, defaulting to GET", aux1);
        return 1;
    }
}

} // namespace fujinet::io::transport::legacy
