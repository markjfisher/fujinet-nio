// tests/test_network_transport_logical_unit_mapping.cpp
//
// This test is to show how future Transport code will be responsible
// for controlling the mapping between a network device id (e.g. N1:) and
// a handle returned from the core network device.
// It is not the responsibility of the core network to manage these mappings.

#include "doctest.h"
#include "net_device_test_helpers.h"

using namespace fujinet::tests::netdev;

namespace {

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
    CHECK(info_req(dev, deviceId, h1).status == StatusCode::InvalidRequest);

    // Closing unit closes the *current* handle (h2)
    CHECK(t.close_unit(1) == StatusCode::Ok);

    // And now h2 should be invalid too
    CHECK(info_req(dev, deviceId, h2).status == StatusCode::InvalidRequest);
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
