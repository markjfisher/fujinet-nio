#include "fujinet/io/devices/network_device.h"

#include "fujinet/io/core/io_message.h"

#include "fujinet/io/devices/net_codec.h"
#include "fujinet/io/devices/net_commands.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/io/devices/network_protocol_registry.h"
#include "fujinet/io/devices/network_protocol_stub.h"

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

NetworkDevice::NetworkDevice()
{
    // Default registrations (core/lib) to unblock usage; platform can override later.
    _registry.register_scheme("http", [] { return std::make_unique<StubNetworkProtocol>(); });
    _registry.register_scheme("https", [] { return std::make_unique<StubNetworkProtocol>(); });
    _registry.register_scheme("tcp", [] { return std::make_unique<StubNetworkProtocol>(); });
}

void NetworkDevice::poll()
{
    for (auto& s : _sessions) {
        if (s.active && s.proto) {
            s.proto->poll();
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

    auto allocate_session = [this](std::uint8_t method, std::uint8_t flags, std::string url,
                                   std::unique_ptr<INetworkProtocol> proto) -> std::uint16_t {
        int chosen = -1;
        for (std::size_t i = 0; i < MAX_SESSIONS; ++i) {
            if (!_sessions[i].active) {
                chosen = static_cast<int>(i);
                break;
            }
        }
        if (chosen < 0) {
            if (proto) {
                proto->close();
            }
            return 0;
        }

        auto& s = _sessions[static_cast<std::size_t>(chosen)];
        s.active = true;
        s.generation = static_cast<std::uint8_t>(s.generation + 1);
        if (s.generation == 0) s.generation = 1;

        s.method = method;
        s.flags = flags;
        s.url = std::move(url);
        s.proto = std::move(proto);

        return make_handle(static_cast<std::uint8_t>(chosen), s.generation);
    };

    auto free_session = [this](Session& s) {
        if (s.proto) {
            s.proto->close();
            s.proto.reset();
        }
        s.active = false;
        s.method = 0;
        s.flags = 0;
        s.url.clear();
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

            const StatusCode st = proto->open(openReq);
            if (st != StatusCode::Ok) {
                resp.status = st;
                return resp;
            }

            const std::uint16_t handle = allocate_session(method, flags, openReq.url, std::move(proto));
            if (handle == 0) {
                // no session slots; best-effort close protocol instance
                resp.status = StatusCode::DeviceBusy;
                return resp;
            }

            // Response: version, flags(bit0 accepted, bit1 needs_body_write), reserved, handle
            std::string out;
            out.reserve(1 + 1 + 2 + 2);

            std::uint8_t oflags = 0x01; // accepted
            const bool needsBodyWrite = (method == 2 /*POST*/ || method == 3 /*PUT*/) && (bodyLenHint > 0);
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

            free_session(*s);

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
            if (eof) flags |= 0x01;
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


