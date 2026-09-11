#include "doctest.h"

#include "fujinet/io/transport/atari_sio_fujibus_framer.h"
#include "fujinet/io/transport/fujibus_transport.h"
#include "fujinet/io/transport/slip_framer.h"
#include "fujibus_wire_fixtures.h"

#include <cstdint>
#include <vector>

using fujinet::io::transport::AtariSioFujiBusFramer;

namespace {

std::vector<std::uint8_t> drain_output(AtariSioFujiBusFramer& framer)
{
    std::vector<std::uint8_t> out;
    std::uint8_t buf[64];
    while (framer.has_output()) {
        const std::size_t n = framer.read_output(buf, sizeof(buf));
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

std::vector<std::uint8_t> drain_request(AtariSioFujiBusFramer& framer)
{
    std::vector<std::uint8_t> out;
    std::uint8_t buf[64];
    while (framer.has_request()) {
        const std::size_t n = framer.read_request(buf, sizeof(buf));
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

std::vector<std::uint8_t> command_frame(std::uint8_t command, std::uint16_t aux)
{
    std::vector<std::uint8_t> frame{
        AtariSioFujiBusFramer::DeviceId,
        command,
        static_cast<std::uint8_t>(aux & 0xFF),
        static_cast<std::uint8_t>((aux >> 8) & 0xFF),
    };
    frame.push_back(AtariSioFujiBusFramer::checksum(frame.data(), frame.size()));
    return frame;
}

} // namespace

namespace {

// Mirrors the SIO channel's stream seam, using the production SIO envelope.
class SioCompositionChannel : public fujinet::io::Channel {
public:
    explicit SioCompositionChannel(AtariSioFujiBusFramer& sio) : _sio(sio) {}
    bool available() override { return _sio.has_request(); }
    std::size_t read(std::uint8_t* buffer, std::size_t capacity) override {
        return _sio.read_request(buffer, capacity);
    }
    void write(const std::uint8_t* buffer, std::size_t length) override {
        _sio.queue_response(buffer, length);
    }
private:
    AtariSioFujiBusFramer& _sio;
};

} // namespace

TEST_CASE("AtariSioFujiBusFramer preserves literal SLIP through raw transport composition")
{
    using namespace fujinet::io;
    namespace fixtures = fujibus_wire_fixtures;
    AtariSioFujiBusFramer sio;
    SioCompositionChannel channel(sio);
    SlipFramer slip;
    FujiBusTransport transport(channel, slip);

    // W, 14 serial bytes; literal command checksum 7F+57+0E = E4.
    const std::vector<std::uint8_t> writeCommand{0x7F, 0x57, 0x0E, 0, 0xE4};
    sio.ingest(writeCommand.data(), writeCommand.size());
    CHECK(drain_output(sio) == std::vector<std::uint8_t>{0x41});
    sio.ingest(fixtures::binary.slip.data(), fixtures::binary.slip.size());
    transport.poll();
    IORequest request;
    CHECK_FALSE(transport.receive(request)); // SIO checksum has not arrived.
    const std::uint8_t requestChecksum = 0xB2; // literal serial sum 6AC -> B2
    sio.ingest(&requestChecksum, 1);
    CHECK(drain_output(sio) == std::vector<std::uint8_t>{0x41, 0x43});
    transport.poll();
    REQUIRE(transport.receive(request));
    CHECK(request.deviceId == 3);
    CHECK(request.command == 4);
    CHECK(request.params.empty());
    CHECK(request.payload == std::vector<std::uint8_t>{0, 0xC0, 0xDB, 0xFF});
    CHECK_FALSE(transport.receive(request));

    for (const bool failed : {false, true}) {
        CAPTURE(failed);
        IOResponse response{};
        response.deviceId = 0xFB;
        response.command = 1;
        response.status = failed ? StatusCode::IOError : StatusCode::Ok;
        response.payload = {0, 0xC0, 0xDB, 0xFF};
        transport.send(response);
        // R, 18 requested bytes; literal command checksum 7F+52+12 = E3.
        const std::vector<std::uint8_t> readCommand{0x7F, 0x52, 0x12, 0, 0xE3};
        sio.ingest(readCommand.data(), readCommand.size());
        std::vector<std::uint8_t> expected{0x41, 0x43};
        const auto& wire = failed ? fixtures::error.slip : fixtures::success.slip;
        expected.insert(expected.end(), wire.begin(), wire.end());
        expected.insert(expected.end(), 3, 0); // SIO padding, outside SLIP frame.
        // Independent serial sums 79B -> A2 and 7A5 -> AC; zero padding adds nothing.
        expected.push_back(failed ? 0xAC : 0xA2);
        CHECK(drain_output(sio) == expected);
    }
}

TEST_CASE("AtariSioFujiBusFramer accepts SIO write payload as raw FujiBus bytes")
{
    AtariSioFujiBusFramer framer;
    const std::vector<std::uint8_t> payload{0xC0, 0x7F, 0x57, 0xC0};

    auto cmd = command_frame(AtariSioFujiBusFramer::CommandWrite, payload.size());
    framer.ingest(cmd.data(), cmd.size());
    CHECK(drain_output(framer) == std::vector<std::uint8_t>{AtariSioFujiBusFramer::SioAck});

    framer.ingest(payload.data(), payload.size());
    const std::uint8_t sum = AtariSioFujiBusFramer::checksum(payload.data(), payload.size());
    framer.ingest(&sum, 1);

    CHECK(drain_output(framer) == std::vector<std::uint8_t>{
                                      AtariSioFujiBusFramer::SioAck,
                                      AtariSioFujiBusFramer::SioComplete,
                                  });
    CHECK(drain_request(framer) == payload);
}

TEST_CASE("AtariSioFujiBusFramer sends padded SIO read response with checksum")
{
    AtariSioFujiBusFramer framer;
    const std::vector<std::uint8_t> response{0xC0, 0x10, 0x20, 0xC0};
    framer.queue_response(response.data(), response.size());

    auto cmd = command_frame(AtariSioFujiBusFramer::CommandRead, 8);
    framer.ingest(cmd.data(), cmd.size());

    const auto out = drain_output(framer);
    REQUIRE(out.size() == 11);
    CHECK(out[0] == AtariSioFujiBusFramer::SioAck);
    CHECK(out[1] == AtariSioFujiBusFramer::SioComplete);
    CHECK(std::vector<std::uint8_t>(out.begin() + 2, out.begin() + 6) == response);
    CHECK(std::vector<std::uint8_t>(out.begin() + 6, out.begin() + 11) ==
          std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x00, AtariSioFujiBusFramer::checksum(out.data() + 2, 8)});
}

TEST_CASE("AtariSioFujiBusFramer handles read command before response is ready")
{
    AtariSioFujiBusFramer framer;
    auto cmd = command_frame(AtariSioFujiBusFramer::CommandRead, 4);
    framer.ingest(cmd.data(), cmd.size());

    CHECK(drain_output(framer) == std::vector<std::uint8_t>{AtariSioFujiBusFramer::SioAck});

    const std::vector<std::uint8_t> response{0x01, 0x02};
    framer.queue_response(response.data(), response.size());

    const auto out = drain_output(framer);
    REQUIRE(out.size() == 6);
    CHECK(out[0] == AtariSioFujiBusFramer::SioComplete);
    CHECK(std::vector<std::uint8_t>(out.begin() + 1, out.begin() + 5) ==
          std::vector<std::uint8_t>{0x01, 0x02, 0x00, 0x00});
    CHECK(out[5] == AtariSioFujiBusFramer::checksum(out.data() + 1, 4));
}

TEST_CASE("AtariSioFujiBusFramer rejects bad command and payload checksums")
{
    AtariSioFujiBusFramer framer;

    auto bad_cmd = command_frame(AtariSioFujiBusFramer::CommandRead, 4);
    bad_cmd.back() ^= 0xFF;
    framer.ingest(bad_cmd.data(), bad_cmd.size());
    CHECK(drain_output(framer) == std::vector<std::uint8_t>{AtariSioFujiBusFramer::SioNak});

    const std::vector<std::uint8_t> payload{0x01, 0x02};
    auto write_cmd = command_frame(AtariSioFujiBusFramer::CommandWrite, payload.size());
    framer.ingest(write_cmd.data(), write_cmd.size());
    CHECK(drain_output(framer) == std::vector<std::uint8_t>{AtariSioFujiBusFramer::SioAck});
    framer.ingest(payload.data(), payload.size());
    std::uint8_t bad_sum = AtariSioFujiBusFramer::checksum(payload.data(), payload.size()) ^ 0xFF;
    framer.ingest(&bad_sum, 1);

    CHECK(drain_output(framer) == std::vector<std::uint8_t>{AtariSioFujiBusFramer::SioNak});
    CHECK_FALSE(framer.has_request());
}

TEST_CASE("AtariSioFujiBusFramer ignores command frames for other SIO devices")
{
    AtariSioFujiBusFramer framer;
    auto cmd = command_frame(AtariSioFujiBusFramer::CommandRead, 4);
    cmd[0] = 0x31; // D1:
    cmd.back() = AtariSioFujiBusFramer::checksum(cmd.data(), 4);

    framer.ingest(cmd.data(), cmd.size());

    CHECK_FALSE(framer.has_output());
    CHECK_FALSE(framer.has_request());
}
