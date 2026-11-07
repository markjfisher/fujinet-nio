#include "doctest.h"

#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/iec_bus.h"
#include "fujinet/io/iec_nio_frontend.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/fujibus_packet_codec.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "iec_test_helpers.h"

#include <string>
#include <vector>

using namespace fujinet::io;
using namespace fujinet::tests;

namespace {

constexpr std::uint8_t nio = IecNioFrontend::default_device;

// Literal FujiBus bytes, so the test does not lean on the codec it exercises: device
// 0xFE, command 0x02, no parameters, payload "hi". Checksum as the protocol folds it.
std::vector<std::uint8_t> literal_request()
{
    std::vector<std::uint8_t> packet = {0xFE, 0x02, 0x08, 0x00, 0x00, 0x00, 'h', 'i'};
    unsigned sum = 0;
    for (const std::uint8_t byte : packet) {
        sum += byte;
        sum = (sum & 0xFF) + (sum >> 8);
    }
    packet[4] = static_cast<std::uint8_t>(sum);
    return packet;
}

class EchoHandler final : public IRequestHandler {
public:
    int calls{0};
    IORequest last{};
    int resets{0};
    const void* resetOwner{nullptr};
    std::size_t responseBytes{3};

    IOResponse handleRequest(const IORequest& request) override
    {
        ++calls;
        last = request;
        IOResponse response = make_success_response(request);
        response.payload.assign(responseBytes, 0x5A);
        return response;
    }
    void resetClient(DeviceID, const void* owner) override
    {
        ++resets;
        resetOwner = owner;
    }
};

struct Rig {
    EchoHandler handler;
    IecNioFrontend frontend{handler};
    IecBus bus;
    Rig() { bus.attach(nio, frontend); }

    void send_packet(const std::vector<std::uint8_t>& packet)
    {
        atn(bus, {listen(nio), data_channel(IecNioFrontend::packet_channel)});
        for (const std::uint8_t byte : packet) {
            CHECK(bus.write(&byte, 1, false) == 1);
        }
        atn(bus, {unlisten});
    }

    ReadOutcome read_response()
    {
        ReadOutcome read = read_all(bus, nio, IecNioFrontend::packet_channel);
        atn(bus, {untalk});
        return read;
    }
};

} // namespace

TEST_CASE("IEC NIO channel: a packet runs once at UNLISTEN and its response reads back whole")
{
    Rig rig;
    const auto packet = literal_request();

    atn(rig.bus, {listen(nio), data_channel(0)});
    for (const std::uint8_t byte : packet) {
        rig.bus.write(&byte, 1, false);
    }
    CHECK(rig.handler.calls == 0);
    atn(rig.bus, {unlisten});
    REQUIRE(rig.handler.calls == 1);
    CHECK(rig.handler.last.deviceId == 0xFE);
    CHECK(rig.handler.last.command == 0x02);
    CHECK(as_text(rig.handler.last.payload) == "hi");
    CHECK(rig.handler.last.clientOwner == &rig.frontend);

    const ReadOutcome read = rig.read_response();
    CHECK(read.eoiOnLastByte);
    REQUIRE(read.bytes.size() >= 6);
    CHECK(read.bytes[0] == 0xFE);
    CHECK(read.bytes[1] == 0x02);
    CHECK((read.bytes[2] | (read.bytes[3] << 8)) == read.bytes.size());
    CHECK(rig.handler.calls == 1);
}

TEST_CASE("IEC NIO channel: a packet may arrive in one write or many")
{
    Rig rig;
    const auto packet = literal_request();
    atn(rig.bus, {listen(nio), data_channel(0)});
    CHECK(rig.bus.write(packet.data(), 3, false) == 3);
    CHECK(rig.bus.write(packet.data() + 3, packet.size() - 3, true) == packet.size() - 3);
    atn(rig.bus, {unlisten});
    CHECK(rig.handler.calls == 1);
}

TEST_CASE("IEC NIO channel: EOI on a data byte does not end the packet, only UNLISTEN does")
{
    Rig rig;
    const auto packet = literal_request();
    atn(rig.bus, {listen(nio), data_channel(0)});
    rig.bus.write(packet.data(), 2, true);
    rig.bus.write(packet.data() + 2, packet.size() - 2, false);
    CHECK(rig.handler.calls == 0);
    atn(rig.bus, {unlisten});
    CHECK(rig.handler.calls == 1);
}

TEST_CASE("IEC NIO channel: addressing it with no data runs nothing and keeps a waiting response")
{
    Rig rig;
    rig.send_packet(literal_request());
    REQUIRE(rig.handler.calls == 1);

    atn(rig.bus, {listen(nio), data_channel(0)});
    atn(rig.bus, {unlisten});
    CHECK(rig.handler.calls == 1);
    CHECK_FALSE(rig.read_response().bytes.empty());
}

TEST_CASE("IEC NIO channel: a read with nothing waiting ends at once")
{
    Rig rig;
    const ReadOutcome read = rig.read_response();
    CHECK(read.bytes.empty());
    CHECK(read.emptyEnd);
}

TEST_CASE("IEC NIO channel: each TALK reads the response from its start, until the next request")
{
    Rig rig;
    rig.send_packet(literal_request());

    const ReadOutcome first = rig.read_response();
    const ReadOutcome second = rig.read_response();
    CHECK_FALSE(first.bytes.empty());
    CHECK(second.bytes == first.bytes);
}

TEST_CASE("IEC NIO channel: a read split across calls delivers the same bytes")
{
    Rig rig;
    rig.handler.responseBytes = 100;
    rig.send_packet(literal_request());

    atn(rig.bus, {talk(nio), data_channel(0)});
    std::vector<std::uint8_t> got;
    for (int guard = 0; guard < 100; ++guard) {
        std::uint8_t chunk[7];
        const IecReadResult read = rig.bus.read(chunk, sizeof(chunk));
        got.insert(got.end(), chunk, chunk + read.bytes);
        if (read.eoi) break;
    }
    atn(rig.bus, {untalk});
    CHECK(got.size() == static_cast<std::size_t>(got[2] | (got[3] << 8)));
}

TEST_CASE("IEC NIO channel: a malformed packet runs nothing and has no response")
{
    Rig rig;
    auto packet = literal_request();
    packet[4] = static_cast<std::uint8_t>(packet[4] + 1); // wrong checksum
    rig.send_packet(packet);
    CHECK(rig.handler.calls == 0);
    CHECK(rig.read_response().emptyEnd);
}

TEST_CASE("IEC NIO channel: a new request replaces a response nobody read")
{
    Rig rig;
    rig.send_packet(literal_request());
    auto bad = literal_request();
    bad[4] = static_cast<std::uint8_t>(bad[4] + 1);
    rig.send_packet(bad);
    CHECK(rig.read_response().emptyEnd);
}

TEST_CASE("IEC NIO channel: a packet over capacity is refused, not run")
{
    Rig rig;
    atn(rig.bus, {listen(nio), data_channel(0)});
    const std::uint8_t byte = 0xFE;
    for (std::size_t i = 0; i < IecNioFrontend::capacity; ++i) {
        REQUIRE(rig.bus.write(&byte, 1, false) == 1);
    }
    CHECK(rig.bus.write(&byte, 1, false) == 0);
    atn(rig.bus, {unlisten});
    CHECK(rig.handler.calls == 0);
    CHECK(rig.read_response().emptyEnd);
}

TEST_CASE("IEC NIO channel: the 512-byte baseline carries a full packet each way")
{
    Rig rig;
    protocol::ByteBuffer big(512, 0x11);
    // A full request, built by hand: header plus payload to exactly 512.
    big[0] = 0xFE;
    big[1] = 0x02;
    big[2] = 0x00;
    big[3] = 0x02;
    big[4] = 0;
    big[5] = 0;
    unsigned sum = 0;
    for (const std::uint8_t b : big) {
        sum += b;
        sum = (sum & 0xFF) + (sum >> 8);
    }
    big[4] = static_cast<std::uint8_t>(sum);
    rig.handler.responseBytes = 400;
    rig.send_packet(big);
    REQUIRE(rig.handler.calls == 1);
    CHECK(rig.handler.last.payload.size() == 506);

    const ReadOutcome read = rig.read_response();
    CHECK(read.bytes.size() <= IecNioFrontend::capacity);
    CHECK(read.eoiOnLastByte);
}

TEST_CASE("IEC NIO channel: a response over capacity becomes an error the client can read")
{
    Rig rig;
    rig.handler.responseBytes = 600;
    rig.send_packet(literal_request());
    const ReadOutcome read = rig.read_response();
    REQUIRE_FALSE(read.bytes.empty());
    CHECK(read.bytes.size() <= IecNioFrontend::capacity);
    CHECK(read.eoiOnLastByte);
}

TEST_CASE("IEC NIO channel: other channels take nothing and send nothing")
{
    Rig rig;
    atn(rig.bus, {listen(nio), data_channel(2)});
    const auto packet = literal_request();
    CHECK(rig.bus.write(packet.data(), packet.size(), false) == 0);
    atn(rig.bus, {unlisten});
    CHECK(rig.handler.calls == 0);

    rig.send_packet(literal_request());
    atn(rig.bus, {talk(nio), data_channel(2)});
    std::uint8_t byte = 0;
    const IecReadResult read = rig.bus.read(&byte, 1);
    CHECK(read.bytes == 0);
    CHECK(read.eoi);
}

TEST_CASE("IEC NIO channel: OPEN and CLOSE change nothing: packets need neither")
{
    Rig rig;
    rig.send_packet(literal_request());
    open_file(rig.bus, nio, 0, "IGNORED");
    atn(rig.bus, {listen(nio), close_channel(0)});
    atn(rig.bus, {unlisten});
    CHECK(rig.handler.calls == 1);
    CHECK_FALSE(rig.read_response().bytes.empty());
}

TEST_CASE("IEC NIO channel: a reset drops the response and the program's network sessions")
{
    Rig rig;
    rig.send_packet(literal_request());
    rig.bus.reset();
    CHECK(rig.handler.resets == 1);
    CHECK(rig.handler.resetOwner == &rig.frontend);
    CHECK(rig.read_response().emptyEnd);
}
