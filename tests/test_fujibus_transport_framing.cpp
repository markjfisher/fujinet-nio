#include "doctest.h"

#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/transport/slip_framer.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/core/channel.h"

#include <deque>
#include <vector>
#include <cstdint>

using namespace fujinet::io;
using namespace fujinet::io::protocol;

// Minimal in-memory Channel — same pattern as test_embed_core.cpp.
class LoopbackChannel : public Channel {
public:
    void push(const std::vector<std::uint8_t>& data) {
        for (auto b : data) _rx.push_back(b);
    }

    bool available() override { return !_rx.empty(); }

    std::size_t read(std::uint8_t* buf, std::size_t maxLen) override {
        std::size_t n = 0;
        while (n < maxLen && !_rx.empty()) {
            buf[n++] = _rx.front();
            _rx.pop_front();
        }
        return n;
    }

    void write(const std::uint8_t* buf, std::size_t len) override {
        for (std::size_t i = 0; i < len; ++i)
            _tx.push_back(buf[i]);
    }

    const std::deque<std::uint8_t>& tx() const { return _tx; }

private:
    std::deque<std::uint8_t> _rx;
    std::deque<std::uint8_t> _tx;
};

// SpyFramer wraps SlipFramer and records whether sendPacket was called.
class SpyFramer : public IFramer {
public:
    void poll(Channel& ch) override                          { _inner.poll(ch); }
    bool nextPacket(ByteBuffer& out) override                { return _inner.nextPacket(out); }
    void sendPacket(Channel& ch, const ByteBuffer& pkt) override {
        sendCalled = true;
        lastPacket = pkt;
        _inner.sendPacket(ch, pkt);
    }
    bool      sendCalled{false};
    ByteBuffer lastPacket;
private:
    SlipFramer _inner;
};

// Build a minimal valid SLIP-framed FujiBus packet and return the raw bytes.
static std::vector<std::uint8_t> make_valid_frame(uint8_t device = 0xFB,
                                                   uint8_t cmd    = 0x01)
{
    FujiBusPacket pkt(static_cast<WireDeviceId>(device), cmd);
    pkt.addParamU8(0); // status = OK
    auto serialized = pkt.serialize();
    return std::vector<std::uint8_t>(serialized.begin(), serialized.end());
}

// Push bytes then poll so _rxBuffer is populated.
static void feed(LoopbackChannel& ch, FujiBusTransport& t,
                 const std::vector<std::uint8_t>& bytes)
{
    ch.push(bytes);
    t.poll();
}

static constexpr uint8_t END = 0xC0;

TEST_SUITE("FujiBusTransport SLIP framing") {

TEST_CASE("normal frame is received correctly") {
    LoopbackChannel ch;
    SlipFramer slipFramer;
    FujiBusTransport t(ch, slipFramer);
    feed(ch, t, make_valid_frame(0xFB, 0x01));

    IORequest req;
    CHECK(t.receive(req));
    CHECK(static_cast<uint8_t>(req.deviceId) == 0xFB);
    CHECK((req.command & 0xFF) == 0x01);
}

TEST_CASE("single stale END before valid frame does not corrupt extraction") {
    // Simulates the condition left by the SLIP_END warm-up experiment:
    // _rxBuffer contains [C0][C0][data][C0] — a lone stale trailing END
    // followed by the next frame's leading END then payload then END.
    LoopbackChannel ch;
    SlipFramer slipFramer;
    FujiBusTransport t(ch, slipFramer);

    auto frame = make_valid_frame(0xFB, 0x02);
    // Prepend a lone stale C0 (the trailing END left from a broken session).
    std::vector<uint8_t> buf;
    buf.push_back(END);       // stale trailing delimiter
    buf.insert(buf.end(), frame.begin(), frame.end());  // [C0][data][C0]
    feed(ch, t, buf);

    IORequest req;
    REQUIRE(t.receive(req));
    CHECK(static_cast<uint8_t>(req.deviceId) == 0xFB);
    CHECK((req.command & 0xFF) == 0x02);
}

TEST_CASE("multiple consecutive ENDs before frame are skipped") {
    LoopbackChannel ch;
    SlipFramer slipFramer;
    FujiBusTransport t(ch, slipFramer);

    auto frame = make_valid_frame(0xFB, 0x03);
    std::vector<uint8_t> buf = { END, END, END };   // three stale delimiters
    buf.insert(buf.end(), frame.begin(), frame.end());
    feed(ch, t, buf);

    IORequest req;
    REQUIRE(t.receive(req));
    CHECK((req.command & 0xFF) == 0x03);
}

TEST_CASE("buffer of only END markers returns false, not a crash or empty frame") {
    LoopbackChannel ch;
    SlipFramer slipFramer;
    FujiBusTransport t(ch, slipFramer);

    feed(ch, t, { END, END, END });

    IORequest req;
    CHECK_FALSE(t.receive(req));
}

TEST_CASE("two valid frames back to back are each received once") {
    LoopbackChannel ch;
    SlipFramer slipFramer;
    FujiBusTransport t(ch, slipFramer);

    auto f1 = make_valid_frame(0xFB, 0x10);
    auto f2 = make_valid_frame(0xFC, 0x20);
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), f1.begin(), f1.end());
    buf.insert(buf.end(), f2.begin(), f2.end());
    feed(ch, t, buf);

    IORequest r1, r2;
    REQUIRE(t.receive(r1));
    REQUIRE(t.receive(r2));
    CHECK(static_cast<uint8_t>(r1.deviceId) == 0xFB);
    CHECK(static_cast<uint8_t>(r2.deviceId) == 0xFC);
}

TEST_CASE("stale END then two valid frames both survive") {
    LoopbackChannel ch;
    SlipFramer slipFramer;
    FujiBusTransport t(ch, slipFramer);

    auto f1 = make_valid_frame(0xFB, 0x11);
    auto f2 = make_valid_frame(0xFC, 0x22);
    std::vector<uint8_t> buf = { END };  // stale
    buf.insert(buf.end(), f1.begin(), f1.end());
    buf.insert(buf.end(), f2.begin(), f2.end());
    feed(ch, t, buf);

    IORequest r1, r2;
    REQUIRE(t.receive(r1));
    REQUIRE(t.receive(r2));
    CHECK((r1.command & 0xFF) == 0x11);
    CHECK((r2.command & 0xFF) == 0x22);
}

TEST_CASE("incomplete frame returns false without corrupting buffer") {
    LoopbackChannel ch;
    SlipFramer slipFramer;
    FujiBusTransport t(ch, slipFramer);

    // Push only the first half of a frame (no terminating END yet).
    auto frame = make_valid_frame(0xFB, 0x05);
    std::vector<uint8_t> partial(frame.begin(), frame.begin() + (frame.size() / 2));
    feed(ch, t, partial);

    IORequest req;
    CHECK_FALSE(t.receive(req));

    // Feed the rest — should now succeed.
    std::vector<uint8_t> rest(frame.begin() + (frame.size() / 2), frame.end());
    feed(ch, t, rest);
    REQUIRE(t.receive(req));
    CHECK((req.command & 0xFF) == 0x05);
}

TEST_CASE("send() routes bytes through IFramer::sendPacket, not directly to channel") {
    // AC: "Given FujiBusTransport sending an IOResponse, when send is called,
    // then bytes are written via the injected framer's sendPacket."
    LoopbackChannel ch;
    SpyFramer spy;
    FujiBusTransport t(ch, spy);

    IOResponse resp;
    resp.id       = 1;
    resp.deviceId = static_cast<DeviceID>(0xFB);
    resp.command  = 0x01;
    resp.status   = StatusCode::Ok;

    t.send(resp);

    // sendPacket on the spy framer must have been called.
    REQUIRE(spy.sendCalled);
    // The packet must be non-empty and have reached the channel.
    CHECK(!spy.lastPacket.empty());
    CHECK(!ch.tx().empty());
    // The framer receives raw bytes; only the channel sees SLIP.
    CHECK(spy.lastPacket == ByteBuffer{0xFB, 0x01, 0x07, 0x00, 0x05, 0x01, 0x00});
    CHECK(ByteBuffer(ch.tx().begin(), ch.tx().end()) ==
          ByteBuffer{0xC0, 0xFB, 0x01, 0x07, 0x00, 0x05, 0x01, 0x00, 0xC0});
}

} // TEST_SUITE

#include "fujibus_wire_fixtures.h"

TEST_CASE("literal FujiBus request maps parameters and binary payload") {
    LoopbackChannel ch;
    SlipFramer framer;
    FujiBusTransport transport(ch, framer);
    feed(ch, transport, fujibus_wire_fixtures::typed.slip);
    IORequest request;
    REQUIRE(transport.receive(request));
    CHECK(request.deviceId == 0x2A);
    CHECK(request.command == 0x63);
    CHECK(request.type == RequestType::Command);
    CHECK(request.params == std::vector<std::uint32_t>{
        0x11, 0x2233, 0x21, 0x22, 0x4455, 0x6677, 0x31,
        0x32, 0x33, 0x01020304, 0x41, 0x42, 0x43, 0x44});
    CHECK(request.payload.empty());

    feed(ch, transport, fujibus_wire_fixtures::binary.slip);
    REQUIRE(transport.receive(request));
    CHECK(request.deviceId == 0x03);
    CHECK(request.command == 0x04);
    CHECK(request.params.empty());
    CHECK(request.payload == ByteBuffer{0x00, 0xC0, 0xDB, 0xFF});
}

TEST_CASE("literal FujiBus response send and receive preserve status and payload") {
    namespace fixtures = fujibus_wire_fixtures;
    for (bool failed : {false, true}) {
        CAPTURE(failed);
        const auto& fixture = failed ? fixtures::error : fixtures::success;
        const auto status = failed ? StatusCode::IOError : StatusCode::Ok;
        LoopbackChannel ch;
        SpyFramer framer;
        FujiBusTransport transport(ch, framer);
        IOResponse outgoing;
        outgoing.deviceId = 0xFB;
        outgoing.command = 0x01;
        outgoing.status = status;
        outgoing.payload = {0x00, 0xC0, 0xDB, 0xFF};
        transport.send(outgoing);
        REQUIRE(framer.sendCalled);
        CHECK(framer.lastPacket == fixture.raw);
        CHECK(ByteBuffer(ch.tx().begin(), ch.tx().end()) == fixture.slip);

        // Receive an independent literal, never bytes captured from send().
        feed(ch, transport, fixture.slip);
        IOResponse incoming;
        REQUIRE(transport.receiveResponse(incoming));
        CHECK(incoming.deviceId == 0xFB);
        CHECK(incoming.command == 0x01);
        CHECK(incoming.status == status);
        CHECK(incoming.payload == ByteBuffer{0x00, 0xC0, 0xDB, 0xFF});
        CHECK_FALSE(transport.receiveResponse(incoming));
    }
}

// Test-only packet source preserves explicit boundaries; it does not model the native stub.
class RawFixtureFramer : public IFramer {
public:
    std::deque<ByteBuffer> packets;
    ByteBuffer sent;
    void poll(Channel&) override {}
    bool nextPacket(ByteBuffer& out) override {
        if (packets.empty()) return false;
        out = std::move(packets.front());
        packets.pop_front();
        return true;
    }
    void sendPacket(Channel&, const ByteBuffer& packet) override { sent = packet; }
};

TEST_CASE("raw and serial framers map independent request fixtures identically") {
    for (const auto* fixture : {&fujibus_wire_fixtures::typed, &fujibus_wire_fixtures::binary}) {
        LoopbackChannel rawChannel, serialChannel;
        RawFixtureFramer raw;
        SlipFramer slip;
        FujiBusTransport rawTransport(rawChannel, raw), serialTransport(serialChannel, slip);
        raw.packets.push_back(fixture->raw);
        feed(serialChannel, serialTransport, fixture->slip);
        IORequest rawRequest, serialRequest;
        REQUIRE(rawTransport.receive(rawRequest));
        REQUIRE(serialTransport.receive(serialRequest));
        CHECK(rawRequest.id == serialRequest.id);
        CHECK(rawRequest.deviceId == serialRequest.deviceId);
        CHECK(rawRequest.command == serialRequest.command);
        CHECK(rawRequest.type == serialRequest.type);
        CHECK(rawRequest.params == serialRequest.params);
        CHECK(rawRequest.payload == serialRequest.payload);
        CHECK_FALSE(rawTransport.receive(rawRequest));
        CHECK_FALSE(serialTransport.receive(serialRequest));
    }
}

TEST_CASE("raw and serial framers preserve literal response status and bytes") {
    for (bool failed : {false, true}) {
        const auto& fixture = failed ? fujibus_wire_fixtures::error : fujibus_wire_fixtures::success;
        LoopbackChannel ch;
        RawFixtureFramer raw;
        FujiBusTransport transport(ch, raw);
        raw.packets.push_back(fixture.raw);
        IOResponse response;
        REQUIRE(transport.receiveResponse(response));
        CHECK(response.deviceId == 0xFB);
        CHECK(response.command == 0x01);
        CHECK(response.status == (failed ? StatusCode::IOError : StatusCode::Ok));
        CHECK(response.payload == ByteBuffer{0x00, 0xC0, 0xDB, 0xFF});
        transport.send(response);
        CHECK(raw.sent == fixture.raw);
        CHECK(ch.tx().empty());
        CHECK_FALSE(transport.receiveResponse(response));
    }
}
