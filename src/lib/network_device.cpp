#include "fujinet/io/devices/network_device.h"

#include "fujinet/io/core/io_message.h"

#include "fujinet/io/devices/net_codec.h"
#include "fujinet/io/devices/net_commands.h"

#include <algorithm>
#include <string>

namespace fujinet::io {

using fujinet::io::netproto::Reader;
using fujinet::io::protocol::NetworkCommand;

static std::vector<std::uint8_t> to_vec(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

void NetworkDevice::poll()
{
    // Stub backend: nothing asynchronous yet.
    // Later (ESP32 backend) this will progress in-flight sessions.
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

            // Consume headers (we store them later; for now just skip safely).
            for (std::uint16_t i = 0; i < headerCount; ++i) {
                std::string_view k, v;
                if (!r.read_lp_u16_string(k) || !r.read_lp_u16_string(v)) {
                    resp.status = StatusCode::InvalidRequest;
                    return resp;
                }
            }

            std::uint32_t bodyLenHint = 0;
            if (!r.read_u32le(bodyLenHint) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            // Allocate a session.
            int chosen = -1;
            for (std::size_t i = 0; i < MAX_SESSIONS; ++i) {
                if (!_sessions[i].active) {
                    chosen = static_cast<int>(i);
                    break;
                }
            }
            if (chosen < 0) {
                resp.status = StatusCode::DeviceBusy;
                return resp;
            }

            auto& s = _sessions[static_cast<std::size_t>(chosen)];
            s.active = true;
            s.generation = static_cast<std::uint8_t>(s.generation + 1);
            if (s.generation == 0) s.generation = 1; // avoid 0 generation if it wraps

            s.method = method;
            s.flags = flags;
            s.url.assign(urlView.data(), urlView.size());

            // Stub backend: pretend the request is immediately completed.
            s.httpStatus = 200;
            s.headers = "Content-Type: text/plain\r\nServer: fujinet-nio-stub\r\n";

            // Fake response body includes the URL so tooling can validate end-to-end.
            const std::string bodyStr = std::string("stub response for: ") + s.url + "\n";
            s.body.assign(bodyStr.begin(), bodyStr.end());
            s.contentLength = static_cast<std::uint64_t>(s.body.size());
            s.eof = false;

            const std::uint16_t handle = make_handle(static_cast<std::uint8_t>(chosen), s.generation);

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

            // Release
            s->active = false;
            s->url.clear();
            s->headers.clear();
            s->body.clear();
            s->httpStatus = 0;
            s->contentLength = 0;
            s->eof = false;

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
            if (!s) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::string out;
            out.reserve(32 + std::min<std::size_t>(s->headers.size(), maxHeaderBytes));

            std::uint8_t flags = 0;
            // bit0=headersIncluded, bit1=hasContentLength
            const std::size_t hdrLen = std::min<std::size_t>(s->headers.size(), maxHeaderBytes);
            if (hdrLen > 0) flags |= 0x01;
            flags |= 0x02; // hasContentLength (stub always has it)

            write_common_prefix(out, NETPROTO_VERSION, flags);
            netproto::write_u16le(out, handle);
            netproto::write_u16le(out, s->httpStatus);
            netproto::write_u64le(out, s->contentLength);
            netproto::write_u16le(out, static_cast<std::uint16_t>(hdrLen));
            out.append(s->headers.data(), hdrLen);

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
            if (!s) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            const std::size_t total = s->body.size();
            const std::size_t off = (offset <= total) ? static_cast<std::size_t>(offset) : total;
            const std::size_t n = std::min<std::size_t>(maxBytes, total - off);
            const bool eof = (off + n) >= total;
            const bool truncated = n < static_cast<std::size_t>(maxBytes);

            std::string out;
            out.reserve(1 + 1 + 2 + 2 + 4 + 2 + n);

            std::uint8_t flags = 0;
            if (eof) flags |= 0x01;
            if (truncated) flags |= 0x02;

            write_common_prefix(out, NETPROTO_VERSION, flags);
            netproto::write_u16le(out, handle);
            netproto::write_u32le(out, offset);
            netproto::write_u16le(out, static_cast<std::uint16_t>(n));
            if (n > 0) {
                netproto::write_bytes(out, s->body.data() + off, n);
            }

            resp.payload = to_vec(out);
            return resp;
        }

        case NetworkCommand::Write:
            // v1 per brief: implement later (POST body streaming).
            return make_base_response(request, StatusCode::Unsupported);

        default:
            return make_base_response(request, StatusCode::Unsupported);
    }
}

} // namespace fujinet::io


