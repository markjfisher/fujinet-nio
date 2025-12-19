#include "doctest.h"

#include "fujinet/core/system_events.h"
#include "fujinet/net/network_link_monitor.h"
#include "fujinet/net/network_events.h"
#include "fujinet/net/network_link.h"

#include <string>
#include <vector>

namespace fujinet::tests {

using fujinet::core::SystemEvents;
using fujinet::net::INetworkLink;
using fujinet::net::LinkState;
using fujinet::net::NetworkEvent;
using fujinet::net::NetworkEventKind;
using fujinet::net::NetworkLinkMonitor;

// -----------------------------------------------------------------------------
// Fake network link for monitor tests
// -----------------------------------------------------------------------------
class FakeNetworkLink final : public INetworkLink {
public:
    LinkState state() const override { return _state; }

    void connect(std::string /*ssid*/, std::string /*pass*/) override
    {
        // tests drive state transitions manually
    }

    void disconnect() override
    {
        // tests drive state transitions manually
    }

    void poll() override
    {
        // no-op; monitor polls us
    }

    std::string ip_address() const override { return _ip; }

    void set_state(LinkState st) { _state = st; }
    void set_ip(std::string ip) { _ip = std::move(ip); }

private:
    LinkState _state{LinkState::Disconnected};
    std::string _ip{};
};

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static std::vector<NetworkEvent> collect_network_events(SystemEvents& events)
{
    std::vector<NetworkEvent> out;
    auto sub = events.network().subscribe([&](const NetworkEvent& ev) { out.push_back(ev); });
    // Return-by-value; caller controls when to unsubscribe (if desired).
    // In most tests we unsubscribe explicitly when we’re done.
    events.network().unsubscribe(sub);
    return out;
}

static const char* kind_name(NetworkEventKind k)
{
    switch (k) {
        case NetworkEventKind::LinkUp:   return "LinkUp";
        case NetworkEventKind::GotIp:    return "GotIp";
        case NetworkEventKind::LinkDown: return "LinkDown";
    }
    return "?";
}

// -----------------------------------------------------------------------------
// EventStream tests (mechanism-level)
// -----------------------------------------------------------------------------
TEST_CASE("EventStream: subscribe/publish delivers; unsubscribe stops delivery")
{
    SystemEvents ev;

    int countA = 0;
    int countB = 0;

    auto subA = ev.network().subscribe([&](const NetworkEvent&) { ++countA; });
    auto subB = ev.network().subscribe([&](const NetworkEvent&) { ++countB; });

    NetworkEvent e;
    e.kind = NetworkEventKind::LinkUp;

    ev.network().publish(e);
    CHECK(countA == 1);
    CHECK(countB == 1);

    ev.network().unsubscribe(subA);

    ev.network().publish(e);
    CHECK(countA == 1); // unchanged
    CHECK(countB == 2); // still receives

    ev.network().unsubscribe(subB);

    ev.network().publish(e);
    CHECK(countA == 1);
    CHECK(countB == 2);
}

TEST_CASE("EventStream: publish uses a snapshot (unsubscribe during publish doesn't break iteration)")
{
    SystemEvents ev;

    int countA = 0;
    int countB = 0;

    fujinet::core::EventStream<NetworkEvent>::Subscription subB{};

    auto subA = ev.network().subscribe([&](const NetworkEvent&) {
        ++countA;
        // Unsubscribe B during A’s callback.
        ev.network().unsubscribe(subB);
    });

    subB = ev.network().subscribe([&](const NetworkEvent&) { ++countB; });

    NetworkEvent e;
    e.kind = NetworkEventKind::LinkUp;

    ev.network().publish(e);

    // Snapshot semantics mean:
    // - both callbacks still run for this publish
    // - B is unsubscribed for future publishes
    CHECK(countA == 1);
    CHECK(countB == 1);

    ev.network().publish(e);
    CHECK(countA == 2);
    CHECK(countB == 1);

    ev.network().unsubscribe(subA);
}

TEST_CASE("EventStream: subscribe during publish does not receive the current event")
{
    SystemEvents ev;

    int countA = 0;
    int countB = 0;

    fujinet::core::EventStream<NetworkEvent>::Subscription subB{};

    auto subA = ev.network().subscribe([&](const NetworkEvent&) {
        ++countA;
        // Subscribe B during callback
        subB = ev.network().subscribe([&](const NetworkEvent&) { ++countB; });
    });

    NetworkEvent e;
    e.kind = NetworkEventKind::LinkUp;

    ev.network().publish(e);

    // B should NOT run during the same publish (snapshot taken before callbacks).
    CHECK(countA == 1);
    CHECK(countB == 0);

    ev.network().publish(e);
    CHECK(countA == 2);
    CHECK(countB == 1);

    ev.network().unsubscribe(subA);
    ev.network().unsubscribe(subB);
}

// -----------------------------------------------------------------------------
// NetworkLinkMonitor tests (behavior-level for transition logic)
// -----------------------------------------------------------------------------
TEST_CASE("NetworkLinkMonitor: no events when link stays Disconnected")
{
    SystemEvents events;
    FakeNetworkLink link;
    NetworkLinkMonitor mon(events, link);

    std::vector<NetworkEvent> got;
    auto sub = events.network().subscribe([&](const NetworkEvent& ev) { got.push_back(ev); });

    mon.poll();
    mon.poll();
    mon.poll();

    CHECK(got.empty());
    events.network().unsubscribe(sub);
}

TEST_CASE("NetworkLinkMonitor: Disconnected -> Connecting emits LinkUp once")
{
    SystemEvents events;
    FakeNetworkLink link;
    NetworkLinkMonitor mon(events, link);

    std::vector<NetworkEvent> got;
    auto sub = events.network().subscribe([&](const NetworkEvent& ev) { got.push_back(ev); });

    link.set_state(LinkState::Disconnected);
    mon.poll();

    link.set_state(LinkState::Connecting);
    mon.poll();

    REQUIRE(got.size() == 1);
    CHECK(got[0].kind == NetworkEventKind::LinkUp);

    // Polling again without state change should not spam LinkUp
    mon.poll();
    CHECK(got.size() == 1);

    events.network().unsubscribe(sub);
}

TEST_CASE("NetworkLinkMonitor: Connecting -> Connected emits GotIp (and LinkUp already emitted earlier)")
{
    SystemEvents events;
    FakeNetworkLink link;
    NetworkLinkMonitor mon(events, link);

    std::vector<NetworkEvent> got;
    auto sub = events.network().subscribe([&](const NetworkEvent& ev) { got.push_back(ev); });

    link.set_state(LinkState::Disconnected);
    link.set_ip("");
    mon.poll();

    // Start connecting
    link.set_state(LinkState::Connecting);
    mon.poll();

    // Now connected
    link.set_state(LinkState::Connected);
    link.set_ip("192.168.1.10");
    mon.poll();

    REQUIRE(got.size() == 2);
    CHECK(got[0].kind == NetworkEventKind::LinkUp);
    CHECK(got[1].kind == NetworkEventKind::GotIp);
    CHECK(got[1].gotIp.ip4 == "192.168.1.10");

    events.network().unsubscribe(sub);
}

TEST_CASE("NetworkLinkMonitor: Connected polls do not re-emit GotIp unless IP changes")
{
    SystemEvents events;
    FakeNetworkLink link;
    NetworkLinkMonitor mon(events, link);

    std::vector<NetworkEvent> got;
    auto sub = events.network().subscribe([&](const NetworkEvent& ev) { got.push_back(ev); });

    // Go straight to Connected (monitor should still emit LinkUp + GotIp)
    link.set_state(LinkState::Connected);
    link.set_ip("10.0.0.2");
    mon.poll();

    REQUIRE(got.size() == 2);
    CHECK(got[0].kind == NetworkEventKind::LinkUp);
    CHECK(got[1].kind == NetworkEventKind::GotIp);

    // Polling without change should not add events
    mon.poll();
    mon.poll();
    CHECK(got.size() == 2);

    // Change IP while connected => GotIp again (new address)
    link.set_ip("10.0.0.3");
    mon.poll();

    REQUIRE(got.size() == 3);
    CHECK(got[2].kind == NetworkEventKind::GotIp);
    CHECK(got[2].gotIp.ip4 == "10.0.0.3");

    events.network().unsubscribe(sub);
}

TEST_CASE("NetworkLinkMonitor: Connected/Connecting -> Disconnected emits LinkDown once")
{
    SystemEvents events;
    FakeNetworkLink link;
    NetworkLinkMonitor mon(events, link);

    std::vector<NetworkEvent> got;
    auto sub = events.network().subscribe([&](const NetworkEvent& ev) { got.push_back(ev); });

    link.set_state(LinkState::Connecting);
    mon.poll(); // should emit LinkUp

    link.set_state(LinkState::Disconnected);
    mon.poll(); // should emit LinkDown

    REQUIRE(got.size() == 2);
    CHECK(got[0].kind == NetworkEventKind::LinkUp);
    CHECK(got[1].kind == NetworkEventKind::LinkDown);

    // Poll again without change: no spam
    mon.poll();
    CHECK(got.size() == 2);

    events.network().unsubscribe(sub);
}

TEST_CASE("NetworkLinkMonitor: Failed behaves like Disconnected for transition rules")
{
    SystemEvents events;
    FakeNetworkLink link;
    NetworkLinkMonitor mon(events, link);

    std::vector<NetworkEvent> got;
    auto sub = events.network().subscribe([&](const NetworkEvent& ev) { got.push_back(ev); });

    link.set_state(LinkState::Failed);
    mon.poll(); // no event expected on first observation (initial last state is Disconnected, but current is Failed)

    // Failed -> Connecting => LinkUp
    link.set_state(LinkState::Connecting);
    mon.poll();

    REQUIRE(got.size() == 1);
    CHECK(got[0].kind == NetworkEventKind::LinkUp);

    // Connecting -> Failed => LinkDown
    link.set_state(LinkState::Failed);
    mon.poll();

    REQUIRE(got.size() == 2);
    CHECK(got[1].kind == NetworkEventKind::LinkDown);

    events.network().unsubscribe(sub);
}

} // namespace fujinet::tests
