#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/net_codec.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/devices/network_protocol_registry.h"
#include "fujinet/io/devices/network_protocol_stub.h"
#include "fujinet/io/protocol/wire_device_ids.h"

namespace fujinet::tests::netdev {

// These are available when another class uses "fujinet::tests::netdev"
using fujinet::io::IORequest;
using fujinet::io::IOResponse;
using fujinet::io::NetworkDevice;
using fujinet::io::StatusCode;

namespace netproto = fujinet::io::netproto;

using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static constexpr std::uint8_t V = 1;

inline std::vector<std::uint8_t> to_vec(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

inline fujinet::io::ProtocolRegistry make_stub_registry_http_only()
{
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("http", [] { return std::make_unique<fujinet::io::StubNetworkProtocol>(); });
    return reg;
}

inline std::uint16_t open_handle_stub(
    NetworkDevice& dev,
    std::uint16_t deviceId,
    const std::string& url,
    std::uint8_t method = 1, // GET
    std::uint8_t flags = 0,
    std::uint32_t bodyLenHint = 0,
    std::initializer_list<std::string_view> responseHeaders = {}
) {
    std::string p;
    netproto::write_u8(p, V);       // version
    netproto::write_u8(p, method);  // method
    netproto::write_u8(p, flags);   // flags
    netproto::write_lp_u16_string(p, url);

    netproto::write_u16le(p, 0); // headerCount (request headers)
    netproto::write_u32le(p, bodyLenHint);

    // NEW: response header allowlist (store nothing if empty)
    netproto::write_u16le(p, static_cast<std::uint16_t>(responseHeaders.size()));
    for (auto h : responseHeaders) {
        netproto::write_lp_u16_string(p, h);
    }

    IORequest req{};
    req.id = 100;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);

    netproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0, oflags = 0;
    std::uint16_t reserved = 0, handle = 0;

    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(oflags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u16le(handle));

    CHECK(ver == V);
    CHECK((oflags & 0x01) != 0); // accepted
    CHECK(handle != 0);

    return handle;
}

inline IOResponse info_req(
    NetworkDevice& dev,
    std::uint16_t deviceId,
    std::uint16_t handle
) {
    std::string ip;
    netproto::write_u8(ip, V);
    netproto::write_u16le(ip, handle);

    IORequest ireq{};
    ireq.id = 200;
    ireq.deviceId = deviceId;
    ireq.command = 0x05; // Info
    ireq.payload = to_vec(ip);

    return dev.handle(ireq);
}

inline IOResponse read_req(
    NetworkDevice& dev,
    std::uint16_t deviceId,
    std::uint16_t handle,
    std::uint32_t offset,
    std::uint16_t maxBytes
) {
    std::string rp;
    netproto::write_u8(rp, V);
    netproto::write_u16le(rp, handle);
    netproto::write_u32le(rp, offset);
    netproto::write_u16le(rp, maxBytes);

    IORequest rreq{};
    rreq.id = 300;
    rreq.deviceId = deviceId;
    rreq.command = 0x02; // Read
    rreq.payload = to_vec(rp);

    return dev.handle(rreq);
}

inline IOResponse write_req(
    NetworkDevice& dev,
    std::uint16_t deviceId,
    std::uint16_t handle,
    std::uint32_t offset,
    std::string_view bytes
) {
    std::string wp;
    netproto::write_u8(wp, V);
    netproto::write_u16le(wp, handle);
    netproto::write_u32le(wp, offset);
    netproto::write_u16le(wp, static_cast<std::uint16_t>(bytes.size()));
    wp.append(bytes.data(), bytes.size());

    IORequest wreq{};
    wreq.id = 400;
    wreq.deviceId = deviceId;
    wreq.command = 0x03; // Write
    wreq.payload = to_vec(wp);

    return dev.handle(wreq);
}

inline IOResponse close_req(
    NetworkDevice& dev,
    std::uint16_t deviceId,
    std::uint16_t handle
) {
    std::string cp;
    netproto::write_u8(cp, V);
    netproto::write_u16le(cp, handle);

    IORequest creq{};
    creq.id = 500;
    creq.deviceId = deviceId;
    creq.command = 0x04; // Close
    creq.payload = to_vec(cp);

    return dev.handle(creq);
}

static std::uint16_t send_open(NetworkDevice& dev, std::uint16_t deviceId, const std::string& url)
{
    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // flags
    netproto::write_lp_u16_string(p, url);
    netproto::write_u16le(p, 0); // headerCount
    netproto::write_u32le(p, 0); // bodyLenHint

    // NEW: response header allowlist = empty (store none)
    netproto::write_u16le(p, 0);

    IORequest req{};
    req.id = 1;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);

    netproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0, flags = 0;
    std::uint16_t reserved = 0, handle = 0;

    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(flags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u16le(handle));

    REQUIRE(ver == V);
    REQUIRE((flags & 0x01) != 0);
    REQUIRE(handle != 0);

    return handle;
}

static StatusCode send_close(NetworkDevice& dev, std::uint16_t deviceId, std::uint16_t handle)
{
    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u16le(p, handle);

    IORequest req{};
    req.id = 2;
    req.deviceId = deviceId;
    req.command = 0x04; // Close
    req.payload = to_vec(p);

    return dev.handle(req).status;
}

} // namespace fujinet::tests::netdev
