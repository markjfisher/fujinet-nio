// tests/test_embed_core.cpp
//
// This file demonstrates and tests using fujinet-nio as an embedded
// library. The host application owns FujinetCore and provides its
// own Channel implementation; fujinet-nio just does IO routing and
// device logic.

#include "doctest.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "fujinet/core/core.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/build/profile.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/virtual_device.h"

using namespace fujinet;

namespace {

// -----------------------------------------------------------------------------
// 1. An in-memory Channel for embedding / tests
// -----------------------------------------------------------------------------

class InMemoryChannel : public io::Channel {
public:
    // Host pushes bytes that should look like they arrived from the wire.
    void pushIncoming(const std::vector<std::uint8_t>& data) {
        for (auto b : data) {
            _rx.push_back(b);
        }
    }

    // Host collects bytes that the core wrote to the wire.
    std::vector<std::uint8_t> takeOutgoing() {
        std::vector<std::uint8_t> out;
        out.reserve(_tx.size());
        while (!_tx.empty()) {
            out.push_back(_tx.front());
            _tx.pop_front();
        }
        return out;
    }

    bool available() override {
        return !_rx.empty();
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
        std::size_t n = 0;
        while (n < maxLen && !_rx.empty()) {
            buffer[n++] = _rx.front();
            _rx.pop_front();
        }
        return n;
    }

    void write(const std::uint8_t* buffer, std::size_t len) override {
        for (std::size_t i = 0; i < len; ++i) {
            _tx.push_back(buffer[i]);
        }
    }

private:
    std::deque<std::uint8_t> _rx;
    std::deque<std::uint8_t> _tx;
};

// -----------------------------------------------------------------------------
// 2. A tiny VirtualDevice used for the test
// -----------------------------------------------------------------------------

class EchoDevice : public io::VirtualDevice {
public:
    io::IOResponse handle(const io::IORequest& req) override {
        io::IOResponse resp;
        resp.id       = req.id;
        resp.deviceId = req.deviceId;
        resp.status   = io::StatusCode::Ok;
        resp.command  = req.command;
        resp.payload  = req.payload; // simple echo
        return resp;
    }

    void poll() override {
        // No background work for this dummy device.
    }
};

} // namespace

// -----------------------------------------------------------------------------
// 3. Integration-style test: core + transport + custom channel
// -----------------------------------------------------------------------------

TEST_CASE("FujinetCore can be embedded with a custom Channel")
{
    // This simulates what an embedding host app would do.

    // 1) Construct the core engine.
    core::FujinetCore core;

    // 2) Decide on a build profile (host may override this if it wants).
    build::BuildProfile profile = build::current_build_profile();
    // For the purposes of this test we don’t really care which machine/
    // channel it claims to be, only that FujiBus transport can be set up.

    // 3) Provide our own Channel implementation.
    InMemoryChannel channel;

    // 4) Install transports based on the profile.
    io::ITransport* primary = core::setup_transports(core, channel, profile);
    CHECK(primary != nullptr);

    // 5) Register a simple VirtualDevice on some DeviceID.
    constexpr io::DeviceID kDeviceId = 0x70; // same as WireDeviceId::FujiNet
    bool registered = core.deviceManager().registerDevice(
        kDeviceId,
        std::make_unique<EchoDevice>()
    );
    CHECK(registered);

    // 6) Drive the core a few ticks. We’re not feeding real FujiBus frames
    //    here yet; this is a smoke test that the engine can be driven and
    //    doesn’t blow up when used as an embeddable component.
    std::uint64_t initialTicks = core.tick_count();
    for (int i = 0; i < 10; ++i) {
        core.tick();
    }
    CHECK(core.tick_count() == initialTicks + 10);

    // 7) Direct-device path: we can also talk to the device manager without
    //    going through a transport, which is another valid embedding style.
    io::IORequest req{};
    req.id       = 123;
    req.deviceId = kDeviceId;
    req.type     = io::RequestType::Command;
    req.command  = 0x42;
    req.payload  = {0xDE, 0xAD, 0xBE, 0xEF};

    io::IOResponse resp = core.deviceManager().handleRequest(req);
    CHECK(resp.id       == req.id);
    CHECK(resp.deviceId == req.deviceId);
    CHECK(resp.status   == io::StatusCode::Ok);
    CHECK(resp.command  == req.command);
    REQUIRE(resp.payload.size() == req.payload.size());
    CHECK(resp.payload == req.payload);

    // This test exercises:
    //  - library-style construction of FujinetCore
    //  - setup_transports() on a caller-provided Channel
    //  - registration and use of VirtualDevice
    //  - tick() as the only required driver API
    //
    // It doubles as documentation for “how do I embed fujinet-nio in my
    // own app/emulator?”
}
