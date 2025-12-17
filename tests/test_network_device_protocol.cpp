// tests/test_network_device_protocol.cpp

#include "doctest.h"

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/net_codec.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/devices/network_protocol_registry.h"
#include "fujinet/io/devices/network_protocol_stub.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
using fujinet::io::IORequest;
using fujinet::io::IOResponse;
using fujinet::io::NetworkDevice;
using fujinet::io::StatusCode;

namespace netproto = fujinet::io::netproto;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static constexpr std::uint8_t V = 1;

static std::vector<std::uint8_t> to_vec(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

static fujinet::io::ProtocolRegistry make_stub_registry_http_only()
{
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("http", [] { return std::make_unique<fujinet::io::StubNetworkProtocol>(); });
    return reg;
}

static std::uint16_t open_handle_stub(
    NetworkDevice& dev,
    std::uint16_t deviceId,
    const std::string& url,
    std::uint8_t method = 1,          // GET
    std::uint8_t flags = 0,
    std::uint32_t bodyLenHint = 0
) {
    std::string p;
    netproto::write_u8(p, V);         // version
    netproto::write_u8(p, method);    // method
    netproto::write_u8(p, flags);     // flags
    netproto::write_lp_u16_string(p, url);
    netproto::write_u16le(p, 0);      // headerCount
    netproto::write_u32le(p, bodyLenHint);

    IORequest req{};
    req.id = 100;
    req.deviceId = deviceId;
    req.command = 0x01;              // Open
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
    CHECK((oflags & 0x01) != 0);      // accepted
    CHECK(handle != 0);

    return handle;
}

static IOResponse info_req(NetworkDevice& dev, std::uint16_t deviceId, std::uint16_t handle, std::uint16_t maxHeaderBytes)
{
    std::string ip;
    netproto::write_u8(ip, V);
    netproto::write_u16le(ip, handle);
    netproto::write_u16le(ip, maxHeaderBytes);

    IORequest ireq{};
    ireq.id = 200;
    ireq.deviceId = deviceId;
    ireq.command = 0x05; // Info
    ireq.payload = to_vec(ip);

    return dev.handle(ireq);
}

static IOResponse read_req(NetworkDevice& dev, std::uint16_t deviceId, std::uint16_t handle, std::uint32_t offset, std::uint16_t maxBytes)
{
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

static IOResponse write_req(NetworkDevice& dev, std::uint16_t deviceId, std::uint16_t handle, std::uint32_t offset, std::string_view bytes)
{
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

static IOResponse close_req(NetworkDevice& dev, std::uint16_t deviceId, std::uint16_t handle)
{
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

} // namespace

TEST_CASE("NetworkDevice v1: Open -> Info -> Read -> Close (stub backend)")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // ---- Open ----
    std::uint16_t handle = 0;
    {
        std::string p;
        netproto::write_u8(p, V); // version
        netproto::write_u8(p, 1); // method=GET
        netproto::write_u8(p, 0); // flags
        netproto::write_lp_u16_string(p, "http://example.com/hello");
        netproto::write_u16le(p, 0); // headerCount
        netproto::write_u32le(p, 0); // bodyLenHint

        IORequest req{};
        req.id = 1;
        req.deviceId = deviceId;
        req.command = 0x01; // Open
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        CHECK(resp.status == StatusCode::Ok);

        REQUIRE(resp.payload.size() >= 1 + 1 + 2 + 2);
        netproto::Reader r(resp.payload.data(), resp.payload.size());

        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u16le(handle));

        CHECK(ver == V);
        CHECK((flags & 0x01) != 0); // accepted
        CHECK(handle != 0);
    }

    // ---- Info ----
    {
        IOResponse iresp = info_req(dev, deviceId, handle, 1024);
        CHECK(iresp.status == StatusCode::Ok);

        netproto::Reader ir(iresp.payload.data(), iresp.payload.size());

        std::uint8_t iver = 0, iflags = 0;
        std::uint16_t ires = 0, ihandle = 0, httpStatus = 0;
        std::uint64_t contentLength = 0;
        std::uint16_t hdrLen = 0;

        REQUIRE(ir.read_u8(iver));
        REQUIRE(ir.read_u8(iflags));
        REQUIRE(ir.read_u16le(ires));
        REQUIRE(ir.read_u16le(ihandle));
        REQUIRE(ir.read_u16le(httpStatus));
        REQUIRE(ir.read_u64le(contentLength));
        REQUIRE(ir.read_u16le(hdrLen));

        CHECK(iver == V);
        CHECK(ihandle == handle);
        CHECK(httpStatus == 200);
        CHECK((iflags & 0x02) != 0); // hasContentLength
        CHECK((iflags & 0x04) != 0); // hasHttpStatus
        CHECK(hdrLen > 0);

        const std::uint8_t* hdrPtr = nullptr;
        REQUIRE(ir.read_bytes(hdrPtr, hdrLen));

        std::string hdr(reinterpret_cast<const char*>(hdrPtr), hdrLen);
        CHECK(hdr.find("Server:") != std::string::npos);
    }

    // ---- Read ----
    {
        IOResponse rresp = read_req(dev, deviceId, handle, 0, 1024);
        CHECK(rresp.status == StatusCode::Ok);

        netproto::Reader rr(rresp.payload.data(), rresp.payload.size());

        std::uint8_t rver = 0, rflags = 0;
        std::uint16_t rres = 0, rhandle = 0;
        std::uint32_t offEcho = 0;
        std::uint16_t dataLen = 0;

        REQUIRE(rr.read_u8(rver));
        REQUIRE(rr.read_u8(rflags));
        REQUIRE(rr.read_u16le(rres));
        REQUIRE(rr.read_u16le(rhandle));
        REQUIRE(rr.read_u32le(offEcho));
        REQUIRE(rr.read_u16le(dataLen));

        CHECK(rver == V);
        CHECK(rhandle == handle);
        CHECK(offEcho == 0);
        CHECK(dataLen > 0);

        const std::uint8_t* dataPtr = nullptr;
        REQUIRE(rr.read_bytes(dataPtr, dataLen));

        std::string body(reinterpret_cast<const char*>(dataPtr), dataLen);
        CHECK(body.find("stub response for: http://example.com/hello") != std::string::npos);

        // eof should be set (stub body is small)
        CHECK((rflags & 0x01) != 0);
    }

    // ---- Close ----
    {
        IOResponse cresp = close_req(dev, deviceId, handle);
        CHECK(cresp.status == StatusCode::Ok);
    }

    // ---- Info after close should be InvalidRequest ----
    {
        IOResponse iresp = info_req(dev, deviceId, handle, 16);
        CHECK(iresp.status == StatusCode::InvalidRequest);
    }
}

TEST_CASE("NetworkDevice v1: Write (POST) returns writtenLen via stub backend")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // ---- Open POST ----
    std::uint16_t handle = 0;
    {
        std::string p;
        netproto::write_u8(p, V); // version
        netproto::write_u8(p, 2); // method=POST
        netproto::write_u8(p, 0); // flags
        netproto::write_lp_u16_string(p, "http://example.com/post");
        netproto::write_u16le(p, 0); // headerCount
        netproto::write_u32le(p, 4); // bodyLenHint

        IORequest req{};
        req.id = 10;
        req.deviceId = deviceId;
        req.command = 0x01; // Open
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);

        netproto::Reader r(resp.payload.data(), resp.payload.size());

        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u16le(handle));

        CHECK(ver == V);
        CHECK((flags & 0x01) != 0); // accepted
        CHECK((flags & 0x02) != 0); // needs_body_write
        CHECK(handle != 0);
    }

    // ---- Write ----
    {
        IOResponse wresp = write_req(dev, deviceId, handle, 0, "ABCD");
        REQUIRE(wresp.status == StatusCode::Ok);

        netproto::Reader wr(wresp.payload.data(), wresp.payload.size());

        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0, h = 0, written = 0;
        std::uint32_t offEcho = 0;

        REQUIRE(wr.read_u8(ver));
        REQUIRE(wr.read_u8(flags));
        REQUIRE(wr.read_u16le(reserved));
        REQUIRE(wr.read_u16le(h));
        REQUIRE(wr.read_u32le(offEcho));
        REQUIRE(wr.read_u16le(written));

        CHECK(ver == V);
        CHECK(h == handle);
        CHECK(offEcho == 0);
        CHECK(written == 4);
    }

    // ---- Close ----
    {
        IOResponse cresp = close_req(dev, deviceId, handle);
        CHECK(cresp.status == StatusCode::Ok);
    }
}

// -----------------------------------------------------------------------------
// Conformance tests (session semantics + StatusCode contract basics)
// -----------------------------------------------------------------------------

TEST_CASE("Conformance: unknown handle => InvalidRequest (Info/Read/Write/Close)")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);
    const std::uint16_t badHandle = 0x1234;

    CHECK(info_req(dev, deviceId, badHandle, 16).status == StatusCode::InvalidRequest);
    CHECK(read_req(dev, deviceId, badHandle, 0, 16).status == StatusCode::InvalidRequest);
    CHECK(write_req(dev, deviceId, badHandle, 0, "AB").status == StatusCode::InvalidRequest);
    CHECK(close_req(dev, deviceId, badHandle).status == StatusCode::InvalidRequest);
}

TEST_CASE("Conformance: handle is invalid after Close, and handle generation changes on reuse")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t h1 = open_handle_stub(dev, deviceId, "http://example.com/a");
    REQUIRE(close_req(dev, deviceId, h1).status == StatusCode::Ok);

    // Old handle must now be rejected
    CHECK(info_req(dev, deviceId, h1, 16).status == StatusCode::InvalidRequest);

    // Next open should not produce the same handle token (generation should change)
    const std::uint16_t h2 = open_handle_stub(dev, deviceId, "http://example.com/b");
    CHECK(h2 != h1);

    REQUIRE(close_req(dev, deviceId, h2).status == StatusCode::Ok);
}

TEST_CASE("Conformance: Open malformed URL => InvalidRequest")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // flags
    netproto::write_lp_u16_string(p, "example.com/no-scheme"); // malformed: missing scheme
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, 0);

    IORequest req{};
    req.id = 600;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    CHECK(resp.status == StatusCode::InvalidRequest);
}

TEST_CASE("Conformance: Open unsupported scheme => Unsupported")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // flags
    netproto::write_lp_u16_string(p, "tcp://example.com:80/");
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, 0);

    IORequest req{};
    req.id = 700;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    CHECK(resp.status == StatusCode::Unsupported);
}

static constexpr std::uint8_t OPEN_ALLOW_EVICT = 0x08;

TEST_CASE("Conformance: capacity strict (allow_evict=0) => 5th Open returns DeviceBusy")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // Fill the session pool. (Design constraint: 4 sessions.)
    (void)open_handle_stub(dev, deviceId, "http://example.com/1", 1, /*flags=*/0, 0);
    (void)open_handle_stub(dev, deviceId, "http://example.com/2", 1, /*flags=*/0, 0);
    (void)open_handle_stub(dev, deviceId, "http://example.com/3", 1, /*flags=*/0, 0);
    (void)open_handle_stub(dev, deviceId, "http://example.com/4", 1, /*flags=*/0, 0);

    // 5th open in strict mode must fail with DeviceBusy.
    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // allow_evict=0
    netproto::write_lp_u16_string(p, "http://example.com/5");
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, 0);

    IORequest req{};
    req.id = 999;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    CHECK(resp.status == StatusCode::DeviceBusy);
}

TEST_CASE("Conformance: capacity eviction (allow_evict=1) => oldest handle becomes invalid")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t h0 = open_handle_stub(dev, deviceId, "http://example.com/h0", 1, OPEN_ALLOW_EVICT, 0);
    REQUIRE(h0 != 0);

    // Apply pressure: opens should keep succeeding (evicting as needed)
    for (int i = 0; i < 32; ++i) {
        (void)open_handle_stub(
            dev,
            deviceId,
            "http://example.com/p/" + std::to_string(i),
            1,
            OPEN_ALLOW_EVICT,
            0
        );
    }

    // The oldest handle should have been evicted at some point.
    CHECK(info_req(dev, deviceId, h0, 16).status == StatusCode::InvalidRequest);
    CHECK(close_req(dev, deviceId, h0).status == StatusCode::InvalidRequest);
}

TEST_CASE("HTTP body lifecycle: Info/Read are NotReady until POST body fully written")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // POST with bodyLenHint > 0 => needs body
    const std::uint16_t h = open_handle_stub(dev, deviceId, "http://example.com/post", /*method=*/2, /*flags=*/0, /*bodyLenHint=*/4);

    // Before body complete, response must not be ready
    CHECK(info_req(dev, deviceId, h, 128).status == StatusCode::NotReady);
    CHECK(read_req(dev, deviceId, h, 0, 128).status == StatusCode::NotReady);

    // Write partial body
    CHECK(write_req(dev, deviceId, h, 0, "AB").status == StatusCode::Ok);

    CHECK(info_req(dev, deviceId, h, 128).status == StatusCode::NotReady);
    CHECK(read_req(dev, deviceId, h, 0, 128).status == StatusCode::NotReady);

    // Finish body
    CHECK(write_req(dev, deviceId, h, 2, "CD").status == StatusCode::Ok);

    // Now response is available (stub should allow Info/Read)
    CHECK(info_req(dev, deviceId, h, 128).status == StatusCode::Ok);
    CHECK(read_req(dev, deviceId, h, 0, 128).status == StatusCode::Ok);

    CHECK(close_req(dev, deviceId, h).status == StatusCode::Ok);
}

TEST_CASE("HTTP body lifecycle: non-sequential Write offset => InvalidRequest")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t h = open_handle_stub(dev, deviceId, "http://example.com/post", /*method=*/2, /*flags=*/0, /*bodyLenHint=*/4);

    // First write at offset 0 OK
    CHECK(write_req(dev, deviceId, h, 0, "AB").status == StatusCode::Ok);

    // Next write must be offset 2; using offset 3 must fail
    CHECK(write_req(dev, deviceId, h, 3, "C").status == StatusCode::InvalidRequest);

    CHECK(close_req(dev, deviceId, h).status == StatusCode::Ok);
}

TEST_CASE("HTTP body lifecycle: Write overflow beyond bodyLenHint => InvalidRequest")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t h = open_handle_stub(dev, deviceId, "http://example.com/post", /*method=*/2, /*flags=*/0, /*bodyLenHint=*/4);

    // Writing 5 bytes when hint is 4 is invalid
    CHECK(write_req(dev, deviceId, h, 0, "ABCDE").status == StatusCode::InvalidRequest);

    CHECK(close_req(dev, deviceId, h).status == StatusCode::Ok);
}

TEST_CASE("HTTP body lifecycle: bodyLenHint>0 on non-POST/PUT => InvalidRequest")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // GET with bodyLenHint should be rejected (keeps v1 simple + deterministic)
    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // flags
    netproto::write_lp_u16_string(p, "http://example.com/get");
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, 4); // bodyLenHint on GET => InvalidRequest

    IORequest req{};
    req.id = 1234;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    CHECK(resp.status == StatusCode::InvalidRequest);
}
