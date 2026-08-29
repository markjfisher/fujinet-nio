#include "doctest.h"

#include "fujinet/io/transport/native_framer.h"
#include "fujinet/io/core/channel.h"

#include <deque>
#include <vector>
#include <cstdint>

using namespace fujinet::io;
using namespace fujinet::io::protocol;

// Minimal in-memory Channel for testing NativeFramer.
class NativeLoopbackChannel : public Channel {
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

TEST_SUITE("NativeFramer") {

    TEST_CASE("single packet round-trip: poll then nextPacket returns true with all bytes") {
        NativeLoopbackChannel ch;
        NativeFramer framer;

        const std::vector<std::uint8_t> payload = {0x01, 0x02, 0x03, 0xAB, 0xFF};
        ch.push(payload);
        framer.poll(ch);

        ByteBuffer out;
        bool got = framer.nextPacket(out);

        CHECK(got);
        REQUIRE(out.size() == payload.size());
        for (std::size_t i = 0; i < payload.size(); ++i) {
            CHECK(out[i] == payload[i]);
        }
    }

    TEST_CASE("no data: nextPacket returns false") {
        NativeLoopbackChannel ch;
        NativeFramer framer;

        // poll on empty channel, then nextPacket
        framer.poll(ch);

        ByteBuffer out;
        bool got = framer.nextPacket(out);

        CHECK_FALSE(got);
        CHECK(out.empty());
    }

    TEST_CASE("nextPacket clears buffer: second call returns false") {
        NativeLoopbackChannel ch;
        NativeFramer framer;

        ch.push({0xDE, 0xAD});
        framer.poll(ch);

        ByteBuffer out;
        CHECK(framer.nextPacket(out));
        CHECK_FALSE(framer.nextPacket(out));
    }

    TEST_CASE("sendPacket writes bytes verbatim to channel") {
        NativeLoopbackChannel ch;
        NativeFramer framer;

        const ByteBuffer packet = {0x10, 0x20, 0x30};
        framer.sendPacket(ch, packet);

        const auto& tx = ch.tx();
        REQUIRE(tx.size() == packet.size());
        for (std::size_t i = 0; i < packet.size(); ++i) {
            CHECK(tx[i] == packet[i]);
        }
    }

    TEST_CASE("sendPacket with empty packet writes nothing") {
        NativeLoopbackChannel ch;
        NativeFramer framer;

        framer.sendPacket(ch, {});
        CHECK(ch.tx().empty());
    }
}
