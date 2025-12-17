// tests/test_network_transport_logical_unit_mapping.cpp
//
// This test is to show how future Transport code will be responsible
// for controlling the mapping between a network device id (e.g. N1:) and
// a handle returned from the core network device.
// It is not the responsibility of the core network to manage these mappings.

#include "doctest.h"

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/net_codec.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/devices/network_protocol_registry.h"
#include "fujinet/io/devices/network_protocol_stub.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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

static std::vector<std::uint8_t> to_vec(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

static fujinet::io::ProtocolRegistry make_stub_registry_http_only()
{
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("http", [] { return std::make_unique<fujinet::io::StubNetworkProtocol>(); });
    return reg;
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

static std::uint16_t send_open(NetworkDevice& dev, std::uint16_t deviceId, const std::string& url)
{
    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // flags
    netproto::write_lp_u16_string(p, url);
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, 0);

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

// Minimal “transport personality”: maps n1..n4 -> active handle.
struct FakeLegacyTransport {
    NetworkDevice& dev;
    std::uint16_t deviceId;

    std::unordered_map<int, std::uint16_t> unitToHandle;

    static std::pair<int, std::string> parse(const std::string& s)
    {
        // "n1:http://foo" -> unit=1, url="http://foo"
        // minimal parsing for tests
        REQUIRE(s.size() >= 3);
        REQUIRE((s[0] == 'n' || s[0] == 'N'));
        REQUIRE((s[1] >= '1' && s[1] <= '4'));
        REQUIRE(s[2] == ':');
        const int unit = s[1] - '0';
        return { unit, s.substr(3) };
    }

    StatusCode open(const std::string& logicalUrl)
    {
        auto [unit, url] = parse(logicalUrl);

        // If client forgot to close, do best-effort close here.
        auto it = unitToHandle.find(unit);
        if (it != unitToHandle.end()) {
            // Close may be Ok or InvalidRequest if already evicted; transport tolerates both.
            StatusCode cs = send_close(dev, deviceId, it->second);
            CHECK((cs == StatusCode::Ok || cs == StatusCode::InvalidRequest));
            unitToHandle.erase(it);
        }

        std::uint16_t h = send_open(dev, deviceId, url);
        unitToHandle[unit] = h;
        return StatusCode::Ok;
    }

    StatusCode close_unit(int unit)
    {
        auto it = unitToHandle.find(unit);
        if (it == unitToHandle.end()) return StatusCode::InvalidRequest;
        StatusCode cs = send_close(dev, deviceId, it->second);
        unitToHandle.erase(it);
        return cs;
    }

    std::uint16_t current_handle_for_unit(int unit) const {
        auto it = unitToHandle.find(unit);
        return (it == unitToHandle.end()) ? 0 : it->second;
    }
    
};

} // namespace

TEST_CASE("Transport mapping: re-opening n1 replaces mapping (new handle becomes active; old handle becomes invalid)")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    FakeLegacyTransport t{ dev, deviceId };

    // Open first URL on n1
    CHECK(t.open("n1:http://foo.bar/baz") == StatusCode::Ok);
    const std::uint16_t h1 = t.current_handle_for_unit(1);
    REQUIRE(h1 != 0);

    // Re-open on same logical unit WITHOUT client close
    CHECK(t.open("n1:http://qux.abc/def") == StatusCode::Ok);
    const std::uint16_t h2 = t.current_handle_for_unit(1);
    REQUIRE(h2 != 0);
    CHECK(h2 != h1); // mapping replaced with a new session/handle

    // Old handle should no longer be a valid session
    // (If it was evicted by close-on-reopen or by LRU, InvalidRequest is correct.)
    CHECK(info_req(dev, deviceId, h1, 16).status == StatusCode::InvalidRequest);

    // Closing unit closes the *current* handle (h2)
    CHECK(t.close_unit(1) == StatusCode::Ok);

    // And now h2 should be invalid too
    CHECK(info_req(dev, deviceId, h2, 16).status == StatusCode::InvalidRequest);
}


TEST_CASE("Transport mapping: n1..n4 can be used concurrently")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    FakeLegacyTransport t{ dev, deviceId };

    CHECK(t.open("n1:http://a/1") == StatusCode::Ok);
    CHECK(t.open("n2:http://a/2") == StatusCode::Ok);
    CHECK(t.open("n3:http://a/3") == StatusCode::Ok);
    CHECK(t.open("n4:http://a/4") == StatusCode::Ok);

    CHECK(t.close_unit(1) == StatusCode::Ok);
    CHECK(t.close_unit(2) == StatusCode::Ok);
    CHECK(t.close_unit(3) == StatusCode::Ok);
    CHECK(t.close_unit(4) == StatusCode::Ok);
}
