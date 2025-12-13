// tests/test_network_device_protocol.cpp
#include "doctest.h"

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/devices/net_codec.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <string>
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

} // namespace

TEST_CASE("NetworkDevice v1: Open -> Info -> Read -> Close (stub backend)")
{
    NetworkDevice dev;

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // ---- Open ----
    {
        std::string p;
        netproto::write_u8(p, V);       // version
        netproto::write_u8(p, 1);       // method=GET
        netproto::write_u8(p, 0);       // flags
        netproto::write_lp_u16_string(p, "http://example.com/hello");
        netproto::write_u16le(p, 0);    // headerCount
        netproto::write_u32le(p, 0);    // bodyLenHint

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
        std::uint16_t reserved = 0, handle = 0;
        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u16le(handle));
        CHECK(ver == V);
        CHECK((flags & 0x01) != 0); // accepted
        CHECK(handle != 0);

        // ---- Info ----
        {
            std::string ip;
            netproto::write_u8(ip, V);
            netproto::write_u16le(ip, handle);
            netproto::write_u16le(ip, 1024); // maxHeaderBytes

            IORequest ireq{};
            ireq.id = 2;
            ireq.deviceId = deviceId;
            ireq.command = 0x05; // Info
            ireq.payload = to_vec(ip);

            IOResponse iresp = dev.handle(ireq);
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
            CHECK(hdrLen > 0);

            const std::uint8_t* hdrPtr = nullptr;
            REQUIRE(ir.read_bytes(hdrPtr, hdrLen));
            std::string hdr(reinterpret_cast<const char*>(hdrPtr), hdrLen);
            CHECK(hdr.find("Server:") != std::string::npos);
        }

        // ---- Read ----
        {
            std::string rp;
            netproto::write_u8(rp, V);
            netproto::write_u16le(rp, handle);
            netproto::write_u32le(rp, 0);    // offset
            netproto::write_u16le(rp, 8);    // maxBytes small to force truncation sometimes

            IORequest rreq{};
            rreq.id = 3;
            rreq.deviceId = deviceId;
            rreq.command = 0x02; // Read
            rreq.payload = to_vec(rp);

            IOResponse rresp = dev.handle(rreq);
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
            CHECK(dataLen <= 8);
        }

        // ---- Close ----
        {
            std::string cp;
            netproto::write_u8(cp, V);
            netproto::write_u16le(cp, handle);

            IORequest creq{};
            creq.id = 4;
            creq.deviceId = deviceId;
            creq.command = 0x04; // Close
            creq.payload = to_vec(cp);

            IOResponse cresp = dev.handle(creq);
            CHECK(cresp.status == StatusCode::Ok);
        }

        // ---- Info after close should be InvalidRequest ----
        {
            std::string ip;
            netproto::write_u8(ip, V);
            netproto::write_u16le(ip, handle);
            netproto::write_u16le(ip, 16);

            IORequest ireq{};
            ireq.id = 5;
            ireq.deviceId = deviceId;
            ireq.command = 0x05; // Info
            ireq.payload = to_vec(ip);

            IOResponse iresp = dev.handle(ireq);
            CHECK(iresp.status == StatusCode::InvalidRequest);
        }
    }
}


