#include "doctest.h"

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

// Helper: verify that a raw SLIP frame starts and ends with END byte.
static bool is_valid_slip_frame(const ByteBuffer& frame) {
    return frame.size() >= 2 &&
           frame.front() == END &&
           frame.back()  == END;
}

TEST_SUITE("SlipFramer") {

TEST_CASE("normal frame is extracted correctly") {
    SlipLoopbackChannel ch;
    SlipFramer framer;
    auto raw = make_valid_frame(0xFB, 0x01);
    feed(ch, framer, raw);

    ByteBuffer out;
    REQUIRE(framer.nextPacket(out));
    CHECK(is_valid_slip_frame(out));
    // The extracted frame bytes should equal the raw serialized frame.
    CHECK(out == ByteBuffer(raw.begin(), raw.end()));
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
    CHECK(is_valid_slip_frame(out));
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
    CHECK(is_valid_slip_frame(out));
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
    CHECK(is_valid_slip_frame(out1));
    CHECK(is_valid_slip_frame(out2));
    // Frames must be different.
    CHECK(out1 != out2);
    // Verify each frame matches its source.
    CHECK(out1 == ByteBuffer(f1.begin(), f1.end()));
    CHECK(out2 == ByteBuffer(f2.begin(), f2.end()));
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
    CHECK(is_valid_slip_frame(out1));
    CHECK(is_valid_slip_frame(out2));
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
    CHECK(is_valid_slip_frame(out));
}

TEST_CASE("sendPacket writes bytes verbatim to channel") {
    SlipLoopbackChannel ch;
    SlipFramer framer;

    ByteBuffer packet = { END, 0x01, 0x02, 0x03, END };
    framer.sendPacket(ch, packet);

    // Verify what was written to channel tx.
    auto& tx = ch.tx();
    REQUIRE(tx.size() == packet.size());
    for (std::size_t i = 0; i < packet.size(); ++i) {
        CHECK(tx[i] == packet[i]);
    }
}

} // TEST_SUITE
