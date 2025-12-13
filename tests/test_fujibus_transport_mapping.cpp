// tests/test_fujibus_transport_mapping.cpp
#include "doctest.h"

#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/io_message.h"

#include <vector>
#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace {

using fujinet::io::Channel;
using fujinet::io::FujiBusTransport;
using fujinet::io::IORequest;
using fujinet::io::IOResponse;
using fujinet::io::StatusCode;
using fujinet::io::RequestType;
using fujinet::io::protocol::FujiBusPacket;
using fujinet::io::protocol::ByteBuffer;
using fujinet::io::protocol::WireDeviceId;

// Minimal fake Channel to feed RX bytes and capture TX bytes.
class FakeChannel final : public Channel {
public:
    bool available() override { return !_rx.empty(); }

    std::size_t read(std::uint8_t* dst, std::size_t maxBytes) override
    {
        const std::size_t n = std::min<std::size_t>(maxBytes, _rx.size());
        std::copy_n(_rx.begin(), n, dst);
        _rx.erase(_rx.begin(), _rx.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    void write(const std::uint8_t* src, std::size_t bytes) override
    {
        _tx.insert(_tx.end(), src, src + bytes);
    }

    void pushRx(ByteBuffer data) { _rx.insert(_rx.end(), data.begin(), data.end()); }
    const ByteBuffer& tx() const { return _tx; }

private:
    ByteBuffer _rx;
    ByteBuffer _tx;
};

} // namespace

TEST_CASE("FujiBusTransport: request params are NOT status (receive maps params verbatim)")
{
    FakeChannel ch;
    FujiBusTransport t(ch);

    // Build a request packet with *two params* that are meaningful to the request
    // (e.g. startIndex, maxEntries). These must not be treated as status.
    const WireDeviceId dev = static_cast<WireDeviceId>(0xFE);
    const std::uint8_t cmd = 0x02; // e.g. FileCommand::ListDirectory

    // Params: startIndex=0, maxEntries=64
    FujiBusPacket reqPkt(dev, cmd,
                         static_cast<std::uint16_t>(0),
                         static_cast<std::uint16_t>(64),
                         ByteBuffer{0xAA, 0xBB}); // payload doesn't matter

    ch.pushRx(reqPkt.serialize());
    t.poll();

    IORequest req{};
    REQUIRE(t.receive(req) == true);

    CHECK(req.deviceId == static_cast<std::uint8_t>(dev));
    CHECK(req.type == RequestType::Command);
    CHECK((req.command & 0xFF) == cmd);

    // This is the key assertion: params survive unchanged.
    REQUIRE(req.params.size() == 2);
    CHECK(req.params[0] == 0);
    CHECK(req.params[1] == 64);

    // Payload survives unchanged too.
    REQUIRE(req.payload.size() == 2);
    CHECK(req.payload[0] == 0xAA);
    CHECK(req.payload[1] == 0xBB);
}

TEST_CASE("FujiBusTransport: responses use param[0] as status (receiveResponse maps status)")
{
    FakeChannel ch;
    FujiBusTransport t(ch);

    // Build a response packet:
    // param[0] = StatusCode (our convention)
    // data     = IOResponse payload
    const WireDeviceId dev = static_cast<WireDeviceId>(0xFE);
    const std::uint8_t cmd = 0x02;

    FujiBusPacket respPkt(dev, cmd,
                          static_cast<std::uint8_t>(StatusCode::Ok),
                          ByteBuffer{0x10, 0x20, 0x30});

    ch.pushRx(respPkt.serialize());
    t.poll();

    IOResponse resp{};

    REQUIRE(t.receiveResponse(resp) == true);

    CHECK(resp.deviceId == static_cast<std::uint8_t>(dev));
    CHECK((resp.command & 0xFF) == cmd);
    CHECK(resp.status == StatusCode::Ok);

    REQUIRE(resp.payload.size() == 3);
    CHECK(resp.payload[0] == 0x10);
    CHECK(resp.payload[1] == 0x20);
    CHECK(resp.payload[2] == 0x30);
}
