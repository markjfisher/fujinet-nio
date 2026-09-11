#include "doctest.h"
#include "fujibus_wire_fixtures.h"

#include "fujinet/io/transport/slip_framer.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/core/channel.h"

#include <deque>
#include <vector>
#include <cstdint>

using namespace fujinet::io;
using namespace fujinet::io::protocol;

// Minimal in-memory Channel — same pattern as test_fujibus_transport_framing.cpp.
class SlipLoopbackChannel : public Channel {
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
static void feed(SlipLoopbackChannel& ch, SlipFramer& framer,
                 const std::vector<std::uint8_t>& bytes)
{
    ch.push(bytes);
    framer.poll(ch);
}

static constexpr uint8_t END = 0xC0;

// Existing extraction fixtures also decode as valid raw FujiBus packets.
static bool is_valid_raw_packet(const ByteBuffer& packet) {
    return FujiBusPacket::fromRaw(packet) != nullptr;
}

TEST_SUITE("SlipFramer") {

TEST_CASE("normal frame is extracted correctly") {
    SlipLoopbackChannel ch;
    SlipFramer framer;
    auto raw = make_valid_frame(0xFB, 0x01);
    feed(ch, framer, raw);

    ByteBuffer out;
    REQUIRE(framer.nextPacket(out));
    CHECK(is_valid_raw_packet(out));
    // The extracted packet excludes serial framing.
    const auto packet = FujiBusPacket::fromSerialized(raw);
    REQUIRE(packet);
    CHECK(out == packet->serializeRaw());
}

TEST_CASE("single stale END before valid frame does not corrupt extraction") {
    SlipLoopbackChannel ch;
    SlipFramer framer;

    auto frame = make_valid_frame(0xFB, 0x02);
    std::vector<uint8_t> buf;
    buf.push_back(END);  // stale trailing delimiter
    buf.insert(buf.end(), frame.begin(), frame.end());
    feed(ch, framer, buf);

    ByteBuffer out;
    REQUIRE(framer.nextPacket(out));
    CHECK(is_valid_raw_packet(out));
}

TEST_CASE("multiple consecutive ENDs before frame are skipped") {
    SlipLoopbackChannel ch;
    SlipFramer framer;

    auto frame = make_valid_frame(0xFB, 0x03);
    std::vector<uint8_t> buf = { END, END, END };   // three stale delimiters
    buf.insert(buf.end(), frame.begin(), frame.end());
    feed(ch, framer, buf);

    ByteBuffer out;
    REQUIRE(framer.nextPacket(out));
    CHECK(is_valid_raw_packet(out));
}

TEST_CASE("buffer of only END markers returns false, not a crash or empty frame") {
    SlipLoopbackChannel ch;
    SlipFramer framer;

    feed(ch, framer, { END, END, END });

    ByteBuffer out;
    CHECK_FALSE(framer.nextPacket(out));
}

TEST_CASE("two valid frames back to back are each extracted once") {
    SlipLoopbackChannel ch;
    SlipFramer framer;

    auto f1 = make_valid_frame(0xFB, 0x10);
    auto f2 = make_valid_frame(0xFC, 0x20);
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), f1.begin(), f1.end());
    buf.insert(buf.end(), f2.begin(), f2.end());
    feed(ch, framer, buf);

    ByteBuffer out1, out2;
    REQUIRE(framer.nextPacket(out1));
    REQUIRE(framer.nextPacket(out2));
    CHECK(is_valid_raw_packet(out1));
    CHECK(is_valid_raw_packet(out2));
    // Frames must be different.
    CHECK(out1 != out2);
    // Verify each frame matches its source.
    const auto packet1 = FujiBusPacket::fromSerialized(f1);
    const auto packet2 = FujiBusPacket::fromSerialized(f2);
    REQUIRE(packet1);
    REQUIRE(packet2);
    CHECK(out1 == packet1->serializeRaw());
    CHECK(out2 == packet2->serializeRaw());
}

TEST_CASE("stale END then two valid frames both survive") {
    SlipLoopbackChannel ch;
    SlipFramer framer;

    auto f1 = make_valid_frame(0xFB, 0x11);
    auto f2 = make_valid_frame(0xFC, 0x22);
    std::vector<uint8_t> buf = { END };  // stale
    buf.insert(buf.end(), f1.begin(), f1.end());
    buf.insert(buf.end(), f2.begin(), f2.end());
    feed(ch, framer, buf);

    ByteBuffer out1, out2;
    REQUIRE(framer.nextPacket(out1));
    REQUIRE(framer.nextPacket(out2));
    CHECK(is_valid_raw_packet(out1));
    CHECK(is_valid_raw_packet(out2));
}

TEST_CASE("incomplete frame returns false; completion returns true") {
    SlipLoopbackChannel ch;
    SlipFramer framer;

    auto frame = make_valid_frame(0xFB, 0x05);
    std::vector<uint8_t> partial(frame.begin(), frame.begin() + (frame.size() / 2));
    feed(ch, framer, partial);

    ByteBuffer out;
    CHECK_FALSE(framer.nextPacket(out));

    // Feed the rest — should now succeed.
    std::vector<uint8_t> rest(frame.begin() + (frame.size() / 2), frame.end());
    feed(ch, framer, rest);
    REQUIRE(framer.nextPacket(out));
    CHECK(is_valid_raw_packet(out));
}

TEST_CASE("framer sends and receives an opaque non-FujiBus packet") {
    SlipLoopbackChannel ch;
    SlipFramer framer;
    const ByteBuffer raw = {0xC0, 0x00, 0xDB, 0xFF};
    const ByteBuffer wire = {0xC0, 0xDB, 0xDC, 0x00, 0xDB, 0xDD, 0xFF, 0xC0};
    framer.sendPacket(ch, raw);
    CHECK(ByteBuffer(ch.tx().begin(), ch.tx().end()) == wire);
    framer.sendPacket(ch, {});
    CHECK(ByteBuffer(ch.tx().begin(), ch.tx().end()) == wire);

    feed(ch, framer, wire);
    ByteBuffer out;
    REQUIRE(framer.nextPacket(out));
    CHECK(out == raw);
    CHECK_FALSE(framer.nextPacket(out));
}

TEST_CASE("escape-only malformed frame is consumed without empty delivery") {
    SlipLoopbackChannel ch;
    SlipFramer framer;
    ByteBuffer wire = {END, 0xDB, 0x01, END};
    const auto& fixture = fujibus_wire_fixtures::binary;
    wire.insert(wire.end(), fixture.slip.begin(), fixture.slip.end());
    feed(ch, framer, wire);
    ByteBuffer out = {0xAA};
    CHECK_FALSE(framer.nextPacket(out));
    CHECK(out.empty());
    REQUIRE(framer.nextPacket(out));
    CHECK(out == fixture.raw);
    CHECK_FALSE(framer.nextPacket(out));
}

TEST_CASE("literal binary frame survives every split and repeated polls") {
    const auto& fixture = fujibus_wire_fixtures::binary;
    for (std::size_t split = 0; split <= fixture.slip.size(); ++split) {
        CAPTURE(split);
        SlipLoopbackChannel ch;
        SlipFramer framer;
        feed(ch, framer, ByteBuffer(fixture.slip.begin(), fixture.slip.begin() + split));
        ByteBuffer out;
        if (split < fixture.slip.size()) {
            for (int poll = 0; poll < 3; ++poll) {
                framer.poll(ch);
                CHECK_FALSE(framer.nextPacket(out));
                CHECK(out.empty());
            }
        }
        feed(ch, framer, ByteBuffer(fixture.slip.begin() + split, fixture.slip.end()));
        REQUIRE(framer.nextPacket(out));
        CHECK(out == fixture.raw);
        CHECK_FALSE(framer.nextPacket(out));
    }
}

TEST_CASE("noise is discarded and literal binary frames retain ordering") {
    SlipLoopbackChannel ch;
    SlipFramer framer;
    ByteBuffer out;
    feed(ch, framer, {0x12, 0xDB, 0x34});
    CHECK_FALSE(framer.nextPacket(out));
    ByteBuffer wire = {0x56, END, END};
    for (const auto* fixture : {&fujibus_wire_fixtures::binary, &fujibus_wire_fixtures::success}) {
        wire.insert(wire.end(), fixture->slip.begin(), fixture->slip.end());
    }
    feed(ch, framer, wire);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == fujibus_wire_fixtures::binary.raw);
    REQUIRE(framer.nextPacket(out));
    CHECK(out == fujibus_wire_fixtures::success.raw);
    CHECK_FALSE(framer.nextPacket(out));
}

TEST_CASE("framer and serial wrapper share escaping and malformed-escape compatibility") {
    const auto& fixture = fujibus_wire_fixtures::binary;
    for (const ByteBuffer escape : {ByteBuffer{0xDB, 0x01}, ByteBuffer{0xDB}}) {
        SlipLoopbackChannel ch;
        SlipFramer framer;
        ByteBuffer wire = fixture.slip;
        wire.insert(wire.end() - 1, escape.begin(), escape.end());
        feed(ch, framer, wire);
        ByteBuffer out;
        REQUIRE(framer.nextPacket(out));
        CHECK(out == fixture.raw);
        const auto legacy = FujiBusPacket::fromSerialized(wire);
        REQUIRE(legacy);
        CHECK(legacy->serializeRaw() == out);
        framer.sendPacket(ch, out);
        CHECK(ByteBuffer(ch.tx().begin(), ch.tx().end()) == fixture.slip);
        CHECK(legacy->serialize() == fixture.slip);
    }
}

} // TEST_SUITE
