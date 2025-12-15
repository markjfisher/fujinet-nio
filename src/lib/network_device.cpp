#include "fujinet/io/devices/network_device.h"

#include "fujinet/io/core/io_message.h"

#include "fujinet/io/devices/net_codec.h"
#include "fujinet/io/devices/net_commands.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/io/devices/network_protocol_registry.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fujinet::io {

using fujinet::io::netproto::Reader;
using fujinet::io::protocol::NetworkCommand;

static std::vector<std::uint8_t> to_vec(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

static std::string to_lower_ascii(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

static bool extract_scheme_lower(std::string_view url, std::string& outSchemeLower)
{
    const auto pos = url.find("://");
    if (pos == std::string_view::npos || pos == 0) {
        return false;
    }
    outSchemeLower = to_lower_ascii(url.substr(0, pos));
    return !outSchemeLower.empty();
}

NetworkDevice::NetworkDevice(ProtocolRegistry registry)
    : _registry(std::move(registry))
{
}

void NetworkDevice::poll()
{
    ++_tickNow;

    for (auto& s : _sessions) {
        if (!s.active || !s.proto) continue;

        // Allow backend to progress (future async backends)
        s.proto->poll();

        // If backend can signal progress/completion later, we can update s.completed
        // and/or touch(s) here when progress is made.
        // const std::uint64_t age  = _tickNow - s.createdTick;
        const std::uint64_t idle = _tickNow - s.lastActivityTick;

        // Reap dead/leaked handles:
        // - Enforce idle timeout
        if (idle > IDLE_TIMEOUT_TICKS) {
            close_and_free(s);
        }
    }
}

static void write_common_prefix(std::string& out, std::uint8_t version, std::uint8_t flags)
{
    netproto::write_u8(out, version);
    netproto::write_u8(out, flags);
    netproto::write_u16le(out, 0); // reserved
}

static bool check_version(Reader& r, std::uint8_t expected)
{
    std::uint8_t ver = 0;
    return r.read_u8(ver) && ver == expected;
}

IOResponse NetworkDevice::handle(const IORequest& request)
{
    auto cmd = protocol::to_network_command(request.command);

    auto session_for_handle = [this](std::uint16_t handle) -> Session* {
        const auto idx = handle_index(handle);
        const auto gen = handle_generation(handle);
        if (idx >= MAX_SESSIONS) return nullptr;
        auto& s = _sessions[idx];
        if (!s.active) return nullptr;
        if (s.generation != gen) return nullptr;
        return &s;
    };

    switch (cmd) {
        case NetworkCommand::Open: {
            auto resp = make_success_response(request);
        
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            std::uint8_t method = 0;
            std::uint8_t flags = 0;
            if (!r.read_u8(method) || !r.read_u8(flags)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            // url: u16 len + bytes
            std::string_view urlView;
            if (!r.read_lp_u16_string(urlView)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            std::uint16_t headerCount = 0;
            if (!r.read_u16le(headerCount)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            std::vector<std::pair<std::string, std::string>> headers;
            headers.reserve(headerCount);
            for (std::uint16_t i = 0; i < headerCount; ++i) {
                std::string_view k, v;
                if (!r.read_lp_u16_string(k) || !r.read_lp_u16_string(v)) {
                    resp.status = StatusCode::InvalidRequest;
                    return resp;
                }
                headers.emplace_back(std::string(k), std::string(v));
            }
        
            std::uint32_t bodyLenHint = 0;
            if (!r.read_u32le(bodyLenHint) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            // Determine URL scheme -> protocol backend
            std::string schemeLower;
            if (!extract_scheme_lower(urlView, schemeLower)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
        
            auto proto = _registry.create(schemeLower);
            if (!proto) {
                resp.status = StatusCode::Unsupported;
                return resp;
            }
        
            NetworkOpenRequest openReq{};
            openReq.method = method;
            openReq.flags = flags;
            openReq.url.assign(urlView.data(), urlView.size());
            openReq.headers = std::move(headers);
            openReq.bodyLenHint = bodyLenHint;
        
            // ---- (C) Reserve slot BEFORE proto->open() ----
            auto reserve_slot = [this]() -> Session* {
                for (auto& s : _sessions) {
                    if (s.active) continue;
        
                    s.active = true; // reserve
                    s.generation = static_cast<std::uint8_t>(s.generation + 1);
                    if (s.generation == 0) s.generation = 1;
        
                    s.createdTick = _tickNow;
                    s.lastActivityTick = _tickNow;
                    s.completed = false;
        
                    // Clear any stale fields just in case
                    s.method = 0;
                    s.flags = 0;
                    s.url.clear();
                    if (s.proto) {
                        s.proto->close();
                        s.proto.reset();
                    }
        
                    return &s;
                }
                return nullptr;
            };
        
            Session* slot = reserve_slot();
        
            // ---- (D) Optional eviction: if busy, evict LRU and retry once ----
            if (!slot) {
                if (auto* victim = pick_lru_victim()) {
                    // We can be stricter here.
                    // e.g. only evict if victim->completed or victim idle > some threshold.
                    // however, entries are pruned after some time anyway
                    close_and_free(*victim);
                    slot = reserve_slot();
                }
            }
        
            if (!slot) {
                // No free slots even after eviction attempt.
                // Best-effort close protocol instance (not strictly necessary since unique_ptr).
                proto->close();
                resp.status = StatusCode::DeviceBusy;
                return resp;
            }
        
            // Actually open the protocol now that we own a slot.
            const StatusCode st = proto->open(openReq);
            if (st != StatusCode::Ok) {
                // Release slot on failure.
                close_and_free(*slot);
                resp.status = st;
                return resp;
            }
        
            // Fill the reserved session now that open succeeded.
            slot->method = method;
            slot->flags = flags;
            slot->url = openReq.url;
            slot->proto = std::move(proto);
            touch(*slot);
        
            auto session_index = [this](const Session* s) -> std::uint8_t {
                // std::array is contiguous; pointer arithmetic is valid.
                return static_cast<std::uint8_t>(s - _sessions.data());
            };

            const std::uint16_t handle = make_handle(session_index(slot), slot->generation);
        
            // Response: version, flags(bit0 accepted, bit1 needs_body_write), reserved, handle
            std::string out;
            out.reserve(1 + 1 + 2 + 2);
        
            std::uint8_t oflags = 0x01; // accepted
            const bool needsBodyWrite =
                (method == 2 /*POST*/ || method == 3 /*PUT*/) && (bodyLenHint > 0);
            if (needsBodyWrite) oflags |= 0x02;
        
            write_common_prefix(out, NETPROTO_VERSION, oflags);
            netproto::write_u16le(out, handle);
        
            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::Close: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            if (!r.read_u16le(handle) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            touch(*s);
            close_and_free(*s);

            // Optional minimal response payload: version + reserved prefix
            std::string out;
            out.reserve(1 + 1 + 2);
            write_common_prefix(out, NETPROTO_VERSION, 0);
            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::Info: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            std::uint16_t maxHeaderBytes = 0;
            if (!r.read_u16le(handle) || !r.read_u16le(maxHeaderBytes) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s || !s->proto) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            touch(*s);

            NetworkInfo info{};
            const StatusCode st = s->proto->info(maxHeaderBytes, info);
            if (st != StatusCode::Ok) {
                resp.status = st;
                return resp;
            }

            std::uint8_t flags = 0;
            // bit0=headersIncluded, bit1=hasContentLength, bit2=hasHttpStatus
            const std::size_t hdrLen = info.headersBlock.size();
            if (hdrLen > 0) flags |= 0x01;
            if (info.hasContentLength) flags |= 0x02;
            if (info.hasHttpStatus) flags |= 0x04;

            std::string out;
            out.reserve(32 + hdrLen);

            write_common_prefix(out, NETPROTO_VERSION, flags);
            netproto::write_u16le(out, handle);
            netproto::write_u16le(out, info.hasHttpStatus ? info.httpStatus : 0);
            netproto::write_u64le(out, info.hasContentLength ? info.contentLength : 0);
            netproto::write_u16le(out, static_cast<std::uint16_t>(hdrLen));
            out.append(info.headersBlock.data(), hdrLen);

            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::Read: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            std::uint32_t offset = 0;
            std::uint16_t maxBytes = 0;
            if (!r.read_u16le(handle) || !r.read_u32le(offset) || !r.read_u16le(maxBytes) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s || !s->proto) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            touch(*s);

            std::vector<std::uint8_t> buf;
            buf.resize(maxBytes);

            std::uint16_t n = 0;
            bool eof = false;
            const StatusCode st = s->proto->read_body(offset, buf.data(), buf.size(), n, eof);
            if (st != StatusCode::Ok) {
                resp.status = st;
                return resp;
            }

            if (n > buf.size()) {
                n = static_cast<std::uint16_t>(buf.size());
            }

            std::string out;
            out.reserve(1 + 1 + 2 + 2 + 4 + 2 + n);

            std::uint8_t flags = 0;
            if (eof) {
                flags |= 0x01;
                s->completed = true;
            }
            if (n < maxBytes) flags |= 0x02; // truncated

            write_common_prefix(out, NETPROTO_VERSION, flags);
            netproto::write_u16le(out, handle);
            netproto::write_u32le(out, offset);
            netproto::write_u16le(out, n);
            if (n > 0 && !buf.empty()) {
                netproto::write_bytes(out, buf.data(), n);
            }

            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::Write: {
            auto resp = make_success_response(request);
            Reader r(request.payload.data(), request.payload.size());
            if (!check_version(r, NETPROTO_VERSION)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::uint16_t handle = 0;
            std::uint32_t offset = 0;
            std::uint16_t dataLen = 0;
            if (!r.read_u16le(handle) || !r.read_u32le(offset) || !r.read_u16le(dataLen)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            const std::uint8_t* dataPtr = nullptr;
            if (!r.read_bytes(dataPtr, dataLen) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            auto* s = session_for_handle(handle);
            if (!s || !s->proto) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            touch(*s);

            std::uint16_t written = 0;
            const StatusCode st = s->proto->write_body(offset,
                                                       dataPtr, dataLen,
                                                       written);
            if (st != StatusCode::Ok) {
                resp.status = st;
                return resp;
            }

            std::string out;
            out.reserve(1 + 1 + 2 + 2 + 4 + 2);
            write_common_prefix(out, NETPROTO_VERSION, 0);
            netproto::write_u16le(out, handle);
            netproto::write_u32le(out, offset);
            netproto::write_u16le(out, written);
            resp.payload = to_vec(out);
            return resp;
        }

        default:
            return make_base_response(request, StatusCode::Unsupported);
    }
}

} // namespace fujinet::io


