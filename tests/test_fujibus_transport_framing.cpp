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
    // Channel bytes must equal what the spy received (framer wrote them verbatim).
    CHECK(ch.tx().size() == spy.lastPacket.size());
}

} // TEST_SUITE
