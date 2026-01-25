// This translation unit is feature-gated. In non-legacy builds we compile it to
// a tiny no-op TU so the linker can drop it (and so it doesn't force code/data
// into memory-constrained targets).
#if defined(FN_BUILD_ATARI_SIO) || defined(FN_BUILD_ATARI_PTY) || defined(FN_BUILD_ATARI_NETSIO) || defined(FN_ENABLE_LEGACY_TRANSPORT)

#include "fujinet/io/legacy/legacy_network_adapter.h"

#include "fujinet/core/logging.h"
#include "fujinet/io/devices/net_codec.h"
#include "fujinet/io/devices/net_commands.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string_view>
#if defined(FN_PLATFORM_POSIX)
#include <thread>
#elif defined(FN_PLATFORM_ESP32) || defined(FN_PLATFORM_ESP32S3)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace fujinet::io::legacy {

static constexpr const char* TAG = "legacy_net_adapter";

using fujinet::io::netproto::Reader;
using fujinet::io::protocol::NetworkCommand;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static void legacy_sleep_ms(std::uint32_t ms)
{
#if defined(FN_PLATFORM_POSIX)
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#elif defined(FN_PLATFORM_ESP32) || defined(FN_PLATFORM_ESP32S3)
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    (void)ms; // no-op fallback
#endif
}

static std::uint8_t get_aux1(const IORequest& req, std::uint8_t def = 0) noexcept
{
    return (req.params.size() >= 1) ? static_cast<std::uint8_t>(req.params[0] & 0xFF) : def;
}

static std::uint8_t get_aux2(const IORequest& req, std::uint8_t def = 0) noexcept
{
    return (req.params.size() >= 2) ? static_cast<std::uint8_t>(req.params[1] & 0xFF) : def;
}

static std::uint16_t get_aux12_le(const IORequest& req, std::uint16_t def = 0) noexcept
{
    const std::uint8_t a1 = get_aux1(req, static_cast<std::uint8_t>(def & 0xFF));
    const std::uint8_t a2 = get_aux2(req, static_cast<std::uint8_t>((def >> 8) & 0xFF));
    return static_cast<std::uint16_t>(a1) | (static_cast<std::uint16_t>(a2) << 8);
}

LegacyNetworkAdapter::LegacyNetworkAdapter(IODeviceManager& deviceManager)
    : _deviceManager(deviceManager)
{
}

std::string LegacyNetworkAdapter::extract_url(const std::vector<std::uint8_t>& payload)
{
    if (payload.empty()) return {};

    std::string url;
    url.reserve(payload.size());

    for (std::uint8_t b : payload) {
        if (b == 0) break;
        url.push_back(static_cast<char>(b));
    }

    // Strip legacy devicespec prefix (e.g. "N:" or "n:")
    if (url.size() >= 2 && url[1] == ':' && (url[0] == 'N' || url[0] == 'n')) {
        url.erase(0, 2);
    }

    return url;
}

std::uint8_t LegacyNetworkAdapter::method_from_aux1(std::uint8_t aux1)
{
    // Legacy aux1 values (from fujinet-firmware Protocol.h):
    // 4=GET (resolve+encode), 5=DELETE, 8=PUT, 12=GET, 13=POST, 14=PUT
    // New protocol: 1=GET,2=POST,3=PUT,4=DELETE,5=HEAD
    switch (aux1) {
        case 4:  return 1; // GET
        case 5:  return 4; // DELETE
        case 8:  return 3; // PUT
        case 9:  return 4; // DELETE
        case 12: return 1; // GET
        case 13: return 2; // POST
        case 14: return 3; // PUT
        default: return 1; // GET (fallback)
    }
}

IORequest LegacyNetworkAdapter::make_open_req(const IORequest& legacyReq, DeviceID legacyDeviceId)
{
    const std::uint8_t aux1 = get_aux1(legacyReq, 12);
    const std::uint8_t aux2 = get_aux2(legacyReq, 0);

    const std::uint8_t method = method_from_aux1(aux1);
    std::uint8_t flags = static_cast<std::uint8_t>(aux2 & 0x07); // legacy: lower bits used for tls/redirects
    // If POST/PUT: use NetworkDevice extension for unknown-length bodies (commit via zero-length Write).
    if (method == 2 || method == 3) {
        flags |= 0x04; // bit2: bodyLenHint==0 but body will be streamed and committed later
    }

    const std::string url = extract_url(legacyReq.payload);

    std::string payload;
    payload.reserve(64 + url.size());
    netproto::write_u8(payload, 1);            // version
    netproto::write_u8(payload, method);       // method
    netproto::write_u8(payload, flags);        // flags
    netproto::write_lp_u16_string(payload, url); // url
    netproto::write_u16le(payload, 0);         // headerCount
    netproto::write_u32le(payload, 0);         // bodyLenHint (legacy unknown)
    netproto::write_u16le(payload, 0);         // respHeaderCount

    IORequest req = legacyReq;
    req.deviceId = to_device_id(WireDeviceId::NetworkService);
    req.command = static_cast<std::uint16_t>(NetworkCommand::Open);
    req.payload.assign(payload.begin(), payload.end());

    // Keep legacy device id in-flight via mapping in the adapter, not in request.
    (void)legacyDeviceId;
    return req;
}

IORequest LegacyNetworkAdapter::make_read_req(const IORequest& legacyReq, const LegacySlot& slot)
{
    const std::uint16_t maxBytes = get_aux12_le(legacyReq, 256);

    std::string payload;
    payload.reserve(1 + 2 + 4 + 2);
    netproto::write_u8(payload, 1);
    netproto::write_u16le(payload, slot.handle);
    netproto::write_u32le(payload, slot.nextReadOffset);
    netproto::write_u16le(payload, maxBytes);

    IORequest req = legacyReq;
    req.deviceId = to_device_id(WireDeviceId::NetworkService);
    req.command = static_cast<std::uint16_t>(NetworkCommand::Read);
    req.payload.assign(payload.begin(), payload.end());
    return req;
}

IORequest LegacyNetworkAdapter::make_write_req(const IORequest& legacyReq, const LegacySlot& slot)
{
    std::string payload;
    payload.reserve(1 + 2 + 4 + 2 + legacyReq.payload.size());
    netproto::write_u8(payload, 1);
    netproto::write_u16le(payload, slot.handle);
    netproto::write_u32le(payload, slot.nextWriteOffset);
    netproto::write_u16le(payload, static_cast<std::uint16_t>(legacyReq.payload.size()));
    if (!legacyReq.payload.empty()) {
        netproto::write_bytes(payload, legacyReq.payload.data(), legacyReq.payload.size());
    }

    IORequest req = legacyReq;
    req.deviceId = to_device_id(WireDeviceId::NetworkService);
    req.command = static_cast<std::uint16_t>(NetworkCommand::Write);
    req.payload.assign(payload.begin(), payload.end());
    return req;
}

IORequest LegacyNetworkAdapter::make_close_req(const IORequest& legacyReq, const LegacySlot& slot)
{
    std::string payload;
    payload.reserve(1 + 2);
    netproto::write_u8(payload, 1);
    netproto::write_u16le(payload, slot.handle);

    IORequest req = legacyReq;
    req.deviceId = to_device_id(WireDeviceId::NetworkService);
    req.command = static_cast<std::uint16_t>(NetworkCommand::Close);
    req.payload.assign(payload.begin(), payload.end());
    return req;
}

IORequest LegacyNetworkAdapter::make_info_req(const IORequest& legacyReq, const LegacySlot& slot)
{
    std::string payload;
    payload.reserve(1 + 2);
    netproto::write_u8(payload, 1);
    netproto::write_u16le(payload, slot.handle);

    IORequest req = legacyReq;
    req.deviceId = to_device_id(WireDeviceId::NetworkService);
    req.command = static_cast<std::uint16_t>(NetworkCommand::Info);
    req.payload.assign(payload.begin(), payload.end());
    return req;
}

static IOResponse legacy_response_like(const IORequest& legacyReq, StatusCode st)
{
    IOResponse r;
    r.id = legacyReq.id;
    r.deviceId = legacyReq.deviceId;
    r.command = legacyReq.command;
    r.status = st;
    return r;
}

IOResponse LegacyNetworkAdapter::convert_open_resp(const IORequest& legacyReq, const IOResponse& newResp, LegacySlot& slot)
{
    IOResponse out = legacy_response_like(legacyReq, newResp.status);

    if (newResp.status != StatusCode::Ok) {
        return out;
    }

    Reader r(newResp.payload.data(), newResp.payload.size());
    std::uint8_t ver = 0, flags = 0;
    std::uint16_t reserved = 0, handle = 0;
    if (!r.read_u8(ver) || !r.read_u8(flags) || !r.read_u16le(reserved) || !r.read_u16le(handle)) {
        out.status = StatusCode::InternalError;
        return out;
    }
    (void)flags;
    (void)reserved;

    slot.handle = handle;
    slot.nextReadOffset = 0;
    slot.nextWriteOffset = 0;
    slot.awaitingCommit = false; // will be set by caller based on method
    out.payload.clear(); // legacy clients do not see handles
    return out;
}

IOResponse LegacyNetworkAdapter::convert_read_resp(const IORequest& legacyReq, const IOResponse& newResp, LegacySlot& slot)
{
    IOResponse out = legacy_response_like(legacyReq, newResp.status);
    if (newResp.status != StatusCode::Ok) {
        return out;
    }

    // Response: ver, flags, reserved, handle, offsetEcho, dataLen, data
    if (newResp.payload.size() < 12) {
        out.status = StatusCode::InternalError;
        return out;
    }

    Reader r(newResp.payload.data(), newResp.payload.size());
    std::uint8_t ver = 0, flags = 0;
    std::uint16_t reserved = 0, handle = 0;
    std::uint32_t offset = 0;
    std::uint16_t dataLen = 0;
    if (!r.read_u8(ver) || !r.read_u8(flags) || !r.read_u16le(reserved) ||
        !r.read_u16le(handle) || !r.read_u32le(offset) || !r.read_u16le(dataLen)) {
        out.status = StatusCode::InternalError;
        return out;
    }

    const std::uint8_t* dataPtr = nullptr;
    if (!r.read_bytes(dataPtr, dataLen)) {
        out.status = StatusCode::InternalError;
        return out;
    }

    out.payload.assign(dataPtr, dataPtr + dataLen);
    slot.nextReadOffset += dataLen;
    (void)flags;
    (void)offset;
    (void)handle;
    return out;
}

IOResponse LegacyNetworkAdapter::convert_write_resp(const IORequest& legacyReq, const IOResponse& newResp, LegacySlot& slot)
{
    IOResponse out = legacy_response_like(legacyReq, newResp.status);
    if (newResp.status != StatusCode::Ok) {
        return out;
    }

    // Response: ver, flags, reserved, handle, offsetEcho, writtenLen
    if (newResp.payload.size() < 12) {
        out.status = StatusCode::InternalError;
        return out;
    }

    Reader r(newResp.payload.data(), newResp.payload.size());
    std::uint8_t ver = 0, flags = 0;
    std::uint16_t reserved = 0, handle = 0;
    std::uint32_t offset = 0;
    std::uint16_t writtenLen = 0;
    if (!r.read_u8(ver) || !r.read_u8(flags) || !r.read_u16le(reserved) ||
        !r.read_u16le(handle) || !r.read_u32le(offset) || !r.read_u16le(writtenLen)) {
        out.status = StatusCode::InternalError;
        return out;
    }

    slot.nextWriteOffset += writtenLen;
    out.payload.clear(); // legacy WRITE returns no payload
    return out;
}

IOResponse LegacyNetworkAdapter::convert_close_resp(const IORequest& legacyReq, const IOResponse& newResp, LegacySlot& slot)
{
    IOResponse out = legacy_response_like(legacyReq, newResp.status);
    if (newResp.status == StatusCode::Ok) {
        slot.handle = 0;
        slot.nextReadOffset = 0;
        slot.nextWriteOffset = 0;
        slot.awaitingCommit = false;
        slot.pendingRead.clear();
        slot.pendingEof = false;
    }
    out.payload.clear();
    return out;
}

IOResponse LegacyNetworkAdapter::convert_info_resp(const IORequest& legacyReq, const IOResponse& newResp, const LegacySlot& slot)
{
    IOResponse out = legacy_response_like(legacyReq, newResp.status);

    // Legacy "not connected" / "not ready" conventions:
    // payload: [bytesWaitingLo, bytesWaitingHi, connected, error]
    auto make_status = [&](std::uint16_t bytesWaiting, std::uint8_t connected, std::uint8_t error) {
        out.status = StatusCode::Ok;
        out.payload = {
            static_cast<std::uint8_t>(bytesWaiting & 0xFF),
            static_cast<std::uint8_t>((bytesWaiting >> 8) & 0xFF),
            connected,
            error
        };
    };

    if (newResp.status == StatusCode::InvalidRequest) {
        make_status(0, 0, 136);
        return out;
    }

    if (newResp.status == StatusCode::NotReady) {
        make_status(0, 0, 136);
        return out;
    }

    if (newResp.status != StatusCode::Ok) {
        make_status(0, 0, 136);
        return out;
    }

    // Info response: ver, flags, reserved, handle, httpStatus, contentLength, hdrLen, headers
    if (newResp.payload.size() < 18) {
        make_status(0, 0, 136);
        return out;
    }

    Reader r(newResp.payload.data(), newResp.payload.size());
    std::uint8_t ver = 0, flags = 0;
    std::uint16_t reserved = 0, handle = 0;
    std::uint16_t httpStatus = 0;
    std::uint64_t contentLength = 0;
    std::uint16_t hdrLen = 0;
    if (!r.read_u8(ver) || !r.read_u8(flags) || !r.read_u16le(reserved) ||
        !r.read_u16le(handle) || !r.read_u16le(httpStatus) || !r.read_u64le(contentLength) ||
        !r.read_u16le(hdrLen)) {
        make_status(0, 0, 136);
        return out;
    }

    // Estimate remaining bytes from content-length if provided.
    std::uint64_t remaining = 0;
    if (contentLength > slot.nextReadOffset) {
        remaining = contentLength - slot.nextReadOffset;
    }
    std::uint16_t bytesWaiting = (remaining > 65535) ? 65535 : static_cast<std::uint16_t>(remaining);

    std::uint8_t connected = (bytesWaiting > 0) ? 1 : 0;

    // Map HTTP status to legacy error byte (mirrors previous bridge mapping).
    std::uint8_t error = 1; // OK
    if (httpStatus == 0) {
        error = 136;
    } else if (httpStatus >= 200 && httpStatus < 300) {
        error = (bytesWaiting == 0) ? 136 : 1;
    } else if (httpStatus == 404 || httpStatus == 410) {
        error = 170; // FILE_NOT_FOUND
    } else if (httpStatus == 401 || httpStatus == 403) {
        error = 165; // INVALID_USERNAME_OR_PASSWORD
    } else if (httpStatus >= 400 && httpStatus < 500) {
        error = 144; // CLIENT_GENERAL
    } else if (httpStatus >= 500) {
        error = 146; // SERVER_GENERAL
    } else {
        error = 136;
    }

    make_status(bytesWaiting, connected, error);
    (void)flags;
    (void)reserved;
    (void)handle;
    (void)hdrLen;
    return out;
}

IOResponse LegacyNetworkAdapter::handleRequest(const IORequest& request)
{
    if (!is_legacy_net_device(request.deviceId)) {
        return _deviceManager.handleRequest(request);
    }

    const std::size_t idx = slot_index(request.deviceId);
    LegacySlot& slot = _slots[idx];

    const std::uint8_t cmd = static_cast<std::uint8_t>(request.command & 0xFF);

    // Legacy command codes
    static constexpr std::uint8_t CMD_OPEN = 'O';
    static constexpr std::uint8_t CMD_CLOSE = 'C';
    static constexpr std::uint8_t CMD_READ = 'R';
    static constexpr std::uint8_t CMD_WRITE = 'W';
    static constexpr std::uint8_t CMD_STATUS = 'S';

    if (cmd == CMD_OPEN) {
        const std::uint8_t aux1 = get_aux1(request, 12);
        const std::uint8_t method = method_from_aux1(aux1);

        IORequest newReq = make_open_req(request, request.deviceId);
        IOResponse newResp = _deviceManager.handleRequest(newReq);
        IOResponse out = convert_open_resp(request, newResp, slot);
        if (out.status == StatusCode::Ok) {
            slot.awaitingCommit = (method == 2 || method == 3); // POST/PUT commit on first STATUS/READ
            slot.pendingRead.clear();
            slot.pendingEof = false;
        }
        return out;
    }

    auto commit_if_needed = [&]() -> StatusCode {
        if (!slot.awaitingCommit) return StatusCode::Ok;

        // Commit by sending a zero-length Write at the next required offset.
        IORequest commitReq = request;
        commitReq.deviceId = to_device_id(WireDeviceId::NetworkService);
        commitReq.command = static_cast<std::uint16_t>(NetworkCommand::Write);

        std::string payload;
        payload.reserve(1 + 2 + 4 + 2);
        netproto::write_u8(payload, 1);
        netproto::write_u16le(payload, slot.handle);
        netproto::write_u32le(payload, slot.nextWriteOffset);
        netproto::write_u16le(payload, 0);
        commitReq.payload.assign(payload.begin(), payload.end());

        IOResponse commitResp = _deviceManager.handleRequest(commitReq);
        if (commitResp.status != StatusCode::Ok) {
            return commitResp.status;
        }

        slot.awaitingCommit = false;
        return StatusCode::Ok;
    };

    if (cmd == CMD_STATUS) {
        if (slot.handle == 0) {
            // Not connected: legacy expects "ok + not connected payload".
            IOResponse out = legacy_response_like(request, StatusCode::Ok);
            out.payload = {0x00, 0x00, 0x00, 136};
            return out;
        }
        if (const StatusCode st = commit_if_needed(); st != StatusCode::Ok) {
            return legacy_response_like(request, st);
        }

        // Legacy clients (notably fujinet-lib's `network_read`) rely on STATUS to:
        // - keep conn=1 while the HTTP transfer is in-flight, even if bw==0
        // - provide a non-zero bw to trigger 'R' reads when bytes are available
        //
        // We must NOT "fake" bw. Instead, we probe the backend using an offset read
        // and cache any returned bytes in slot.pendingRead so the next 'R' can
        // deliver exactly those bytes to the legacy client.
        auto make_status = [&](std::uint16_t bytesWaiting, std::uint8_t connected, std::uint8_t error) {
            IOResponse out = legacy_response_like(request, StatusCode::Ok);
            out.payload = {
                static_cast<std::uint8_t>(bytesWaiting & 0xFF),
                static_cast<std::uint8_t>((bytesWaiting >> 8) & 0xFF),
                connected,
                error
            };
            return out;
        };

        // If we already have cached bytes waiting, report them.
        if (!slot.pendingRead.empty()) {
            const std::uint16_t bw = (slot.pendingRead.size() > 65535) ? 65535 : static_cast<std::uint16_t>(slot.pendingRead.size());
            return make_status(bw, 1, 1);
        }
        if (slot.pendingEof) {
            return make_status(0, 0, 136);
        }

        // Probe availability at the current cursor (does not change slot.nextReadOffset).
        IORequest probeReq = request;
        probeReq.deviceId = to_device_id(WireDeviceId::NetworkService);
        probeReq.command = static_cast<std::uint16_t>(NetworkCommand::Read);

        std::string pld;
        pld.reserve(1 + 2 + 4 + 2);
        netproto::write_u8(pld, 1);
        netproto::write_u16le(pld, slot.handle);
        netproto::write_u32le(pld, slot.nextReadOffset);
        netproto::write_u16le(pld, 512); // probe up to 512 bytes currently available
        probeReq.payload.assign(pld.begin(), pld.end());

        IOResponse probeResp = _deviceManager.handleRequest(probeReq);
        if (probeResp.status == StatusCode::NotReady) {
            // Transfer in-flight, no bytes yet.
            return make_status(0, 1, 1);
        }
        if (probeResp.status != StatusCode::Ok) {
            // Hard error.
            return make_status(0, 0, 136);
        }

        if (probeResp.payload.size() < 12) {
            return make_status(0, 0, 136);
        }

        Reader r(probeResp.payload.data(), probeResp.payload.size());
        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0, handle = 0;
        std::uint32_t offset = 0;
        std::uint16_t dataLen = 0;
        if (!r.read_u8(ver) || !r.read_u8(flags) || !r.read_u16le(reserved) ||
            !r.read_u16le(handle) || !r.read_u32le(offset) || !r.read_u16le(dataLen)) {
            return make_status(0, 0, 136);
        }

        const bool eof = (flags & 0x01) != 0;

        const std::uint8_t* dataPtr = nullptr;
        if (!r.read_bytes(dataPtr, dataLen)) {
            return make_status(0, 0, 136);
        }

        if (dataLen > 0) {
            slot.pendingRead.assign(dataPtr, dataPtr + dataLen);
            slot.pendingEof = eof; // may be final bytes
            return make_status(dataLen, 1, 1);
        }

        if (eof) {
            slot.pendingEof = true;
            return make_status(0, 0, 136);
        }

        // No bytes right now, but transfer is still in flight.
        return make_status(0, 1, 1);
    }

    // Remaining commands require an open handle.
    if (slot.handle == 0) {
        IOResponse out = legacy_response_like(request, StatusCode::InvalidRequest);
        return out;
    }

    if (cmd == CMD_READ) {
        if (const StatusCode st = commit_if_needed(); st != StatusCode::Ok) {
            return legacy_response_like(request, st);
        }

        // Legacy client behaviour expects "read" to wait for data (and often to block
        // until the requested length is satisfied), whereas NetworkDevice may return
        // NotReady while the HTTP transfer is still in flight.
        //
        // To preserve legacy semantics (and enable slow/chunked servers), we poll
        // the underlying NetworkDevice Read until:
        // - we have at least 1 byte (then we keep going while progress continues), or
        // - EOF, or
        // - idle timeout elapses without progress (return what we have; if none, Timeout).
        const std::uint16_t wantBytes = get_aux12_le(request, 256);
        std::vector<std::uint8_t> collected;
        collected.reserve(std::min<std::uint16_t>(wantBytes, 1024));

        // Drain any cached bytes first (populated by STATUS probing).
        if (!slot.pendingRead.empty()) {
            const std::size_t take = std::min<std::size_t>(wantBytes, slot.pendingRead.size());
            collected.insert(collected.end(), slot.pendingRead.begin(), slot.pendingRead.begin() + static_cast<std::ptrdiff_t>(take));
            slot.pendingRead.erase(slot.pendingRead.begin(), slot.pendingRead.begin() + static_cast<std::ptrdiff_t>(take));
            slot.nextReadOffset += static_cast<std::uint32_t>(take);
        }

        const auto hardDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        auto idleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);

        auto make_read_req_with_max = [&](std::uint16_t maxBytes) -> IORequest {
            std::string payload;
            payload.reserve(1 + 2 + 4 + 2);
            netproto::write_u8(payload, 1);
            netproto::write_u16le(payload, slot.handle);
            netproto::write_u32le(payload, slot.nextReadOffset);
            netproto::write_u16le(payload, maxBytes);

            IORequest req = request;
            req.deviceId = to_device_id(WireDeviceId::NetworkService);
            req.command = static_cast<std::uint16_t>(NetworkCommand::Read);
            req.payload.assign(payload.begin(), payload.end());
            return req;
        };

        bool eof = false;

        while (collected.size() < wantBytes && std::chrono::steady_clock::now() < hardDeadline) {
            const std::uint16_t remaining = static_cast<std::uint16_t>(wantBytes - collected.size());
            IORequest newReq = make_read_req_with_max(remaining);
            IOResponse newResp = _deviceManager.handleRequest(newReq);

            if (newResp.status == StatusCode::Ok) {
                // Parse NetworkDevice Read response: ver, flags, reserved, handle, offsetEcho, dataLen, data
                if (newResp.payload.size() < 12) {
                    return legacy_response_like(request, StatusCode::InternalError);
                }

                Reader r(newResp.payload.data(), newResp.payload.size());
                std::uint8_t ver = 0, flags = 0;
                std::uint16_t reserved = 0, handle = 0;
                std::uint32_t offset = 0;
                std::uint16_t dataLen = 0;
                if (!r.read_u8(ver) || !r.read_u8(flags) || !r.read_u16le(reserved) ||
                    !r.read_u16le(handle) || !r.read_u32le(offset) || !r.read_u16le(dataLen)) {
                    return legacy_response_like(request, StatusCode::InternalError);
                }

                const std::uint8_t* dataPtr = nullptr;
                if (!r.read_bytes(dataPtr, dataLen)) {
                    return legacy_response_like(request, StatusCode::InternalError);
                }

                eof = (flags & 0x01) != 0;

                if (dataLen > 0) {
                    collected.insert(collected.end(), dataPtr, dataPtr + dataLen);
                    slot.nextReadOffset += dataLen;
                    idleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
                } else {
                    // No bytes this iteration; if transfer is still running, we'll see NotReady next time.
                }

                if (eof) {
                    slot.pendingEof = true;
                    break;
                }

                // If no progress and we've exceeded idle, stop waiting.
                if (collected.empty() && std::chrono::steady_clock::now() > idleDeadline) {
                    break;
                }
                continue;
            }

            if (newResp.status == StatusCode::NotReady) {
                if (!collected.empty()) {
                    // Already have some data; keep going while server makes progress.
                    if (std::chrono::steady_clock::now() > idleDeadline) {
                        break;
                    }
                } else {
                    // No data yet; still wait a bit for first bytes.
                    if (std::chrono::steady_clock::now() > idleDeadline) {
                        break;
                    }
                }

                legacy_sleep_ms(10);
                continue;
            }

            // Hard error.
            return legacy_response_like(request, newResp.status);
        }

        if (collected.empty() && !eof) {
            return legacy_response_like(request, StatusCode::Timeout);
        }

        IOResponse out = legacy_response_like(request, StatusCode::Ok);
        out.payload = std::move(collected);
        return out;
    }

    if (cmd == CMD_WRITE) {
        IORequest newReq = make_write_req(request, slot);
        IOResponse newResp = _deviceManager.handleRequest(newReq);
        return convert_write_resp(request, newResp, slot);
    }

    if (cmd == CMD_CLOSE) {
        IORequest newReq = make_close_req(request, slot);
        IOResponse newResp = _deviceManager.handleRequest(newReq);
        return convert_close_resp(request, newResp, slot);
    }

    FN_LOGW(TAG, "Unsupported legacy network command 0x%02X on device 0x%02X", cmd, request.deviceId);
    return legacy_response_like(request, StatusCode::Unsupported);
}

} // namespace fujinet::io::legacy

#else
// Non-legacy build: intentionally empty.
#endif

