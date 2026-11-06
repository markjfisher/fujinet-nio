#include "doctest.h"

#include "d64_fixture.h"
#include "fake_fs.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/iec_device_set.h"
#include "fujinet/io/netiec_ingress.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace fujinet::io;
using fujinet::tests::D64Fixture;

namespace {

using Bytes = std::vector<std::uint8_t>;

class StubDevice final : public IIecDevice {
public:
    std::string log;
    std::vector<std::uint8_t> toSend;
    bool lastEoi{false};

    void open(std::uint8_t, const std::uint8_t*, std::size_t) override { log += "open "; }
    void close(std::uint8_t) override { log += "close "; }
    void listen(std::uint8_t) override { log += "listen "; }
    std::size_t receive(const std::uint8_t*, std::size_t length, bool eoi) override
    {
        lastEoi = eoi;
        log += "data" + std::to_string(length) + " ";
        return length;
    }
    void unlisten() override { log += "unlisten "; }
    void talk(std::uint8_t) override { log += "talk "; }
    IecReadResult send(std::uint8_t* dst, std::size_t maxBytes) override
    {
        const std::size_t n = toSend.size() < maxBytes ? toSend.size() : maxBytes;
        std::copy_n(toSend.begin(), n, dst);
        toSend.erase(toSend.begin(), toSend.begin() + static_cast<std::ptrdiff_t>(n));
        return IecReadResult{n, toSend.empty()};
    }
    void untalk() override { log += "untalk "; }
    void bus_reset() override { log += "reset "; }
};

Bytes frame(std::uint8_t command, const Bytes& payload)
{
    Bytes out = {command, static_cast<std::uint8_t>(payload.size() & 0xFF),
                 static_cast<std::uint8_t>(payload.size() >> 8)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

struct Rig {
    IecBus bus;
    StubDevice device;
    NetIecServer server{bus, "NAME"};
    Rig() { bus.attach(8, device); }

    Bytes exchange(std::uint8_t command, const Bytes& payload)
    {
        Bytes out;
        const Bytes request = frame(command, payload);
        server.receive(request.data(), request.size(), out);
        return out;
    }
};

} // namespace

TEST_CASE("NetIEC: HELLO answers version, device mask and name")
{
    Rig rig;
    const Bytes reply = rig.exchange(0x01, {1, 'V', 'I', 'C', 'E'});
    const Bytes expected = {0x81, 9, 0, 1, 0x00, 0x01, 0x00, 0x00, 'N', 'A', 'M', 'E'};
    CHECK(reply == expected);
}

TEST_CASE("NetIEC: HELLO leaves devices as they are, but drops an unfinished transaction")
{
    Rig rig;
    (void)rig.exchange(0x02, {0x28, 0xF0});
    (void)rig.exchange(0x03, {0, 'X'});
    (void)rig.exchange(0x01, {1});
    (void)rig.exchange(0x02, {0x3F});
    CHECK(rig.device.log.empty());
}

TEST_CASE("NetIEC: ATN answers acknowledged or not present")
{
    Rig rig;
    CHECK(rig.exchange(0x02, {0x28, 0x60}) == Bytes{0x82, 1, 0, 0});
    CHECK(rig.device.log == "listen ");
    CHECK(rig.exchange(0x02, {0x3F}) == Bytes{0x82, 1, 0, 0});
    CHECK(rig.exchange(0x02, {0x29, 0x60}) == Bytes{0x82, 1, 0, 1});
    CHECK(rig.exchange(0x02, {}) == Bytes{0x82, 1, 0, 1});
}

TEST_CASE("NetIEC: WRITE answers the bytes taken and carries the EOI flag")
{
    Rig rig;
    (void)rig.exchange(0x02, {0x28, 0x60});
    CHECK(rig.exchange(0x03, {0x00, 1, 2, 3}) == Bytes{0x83, 2, 0, 3, 0});
    CHECK_FALSE(rig.device.lastEoi);
    CHECK(rig.exchange(0x03, {0x01, 9}) == Bytes{0x83, 2, 0, 1, 0});
    CHECK(rig.device.lastEoi);
    // Nobody listening, and a WRITE with not even its flags byte: nothing taken.
    (void)rig.exchange(0x02, {0x3F});
    CHECK(rig.exchange(0x03, {0x00, 1}) == Bytes{0x83, 2, 0, 0, 0});
    CHECK(rig.exchange(0x03, {}) == Bytes{0x83, 2, 0, 0, 0});
}

TEST_CASE("NetIEC: READ returns what was asked for, with EOI on the end of the stream")
{
    Rig rig;
    rig.device.toSend = {0xAA, 0xBB, 0xCC};
    (void)rig.exchange(0x02, {0x48, 0x60});
    CHECK(rig.exchange(0x04, {2, 0}) == Bytes{0x84, 3, 0, 0x00, 0xAA, 0xBB});
    CHECK(rig.exchange(0x04, {2, 0}) == Bytes{0x84, 2, 0, 0x01, 0xCC});
    CHECK(rig.exchange(0x04, {2, 0}) == Bytes{0x84, 1, 0, 0x01});
}

TEST_CASE("NetIEC: READ with no talker ends at once; RESET reaches the devices")
{
    Rig rig;
    CHECK(rig.exchange(0x04, {1, 0}) == Bytes{0x84, 1, 0, 0x01});
    CHECK(rig.exchange(0x05, {}) == Bytes{0x85, 0, 0});
    CHECK(rig.device.log == "reset ");
}

TEST_CASE("NetIEC: an unknown command is answered, and the stream stays in step")
{
    Rig rig;
    CHECK(rig.exchange(0x7E, {1, 2}) == Bytes{0xFF, 1, 0, 0x7E});
    CHECK(rig.exchange(0x02, {0x28, 0x60}) == Bytes{0x82, 1, 0, 0});
}

TEST_CASE("NetIEC: requests arriving in pieces, or together, get one reply each in order")
{
    Rig rig;
    Bytes stream = frame(0x02, {0x28, 0x60});
    const Bytes second = frame(0x03, {0x00, 7});
    stream.insert(stream.end(), second.begin(), second.end());

    Bytes out;
    for (const std::uint8_t byte : stream) {
        rig.server.receive(&byte, 1, out);
    }
    const Bytes expected = {0x82, 1, 0, 0, 0x83, 2, 0, 1, 0};
    CHECK(out == expected);

    out.clear();
    rig.server.receive(stream.data(), stream.size(), out);
    CHECK(out == expected);
}

namespace {

// A Channel over two byte queues, with what the far end sent available at once.
class QueueChannel final : public Channel {
public:
    std::deque<std::uint8_t>* inbound;
    Bytes* outbound;
    QueueChannel(std::deque<std::uint8_t>& in, Bytes& out) : inbound(&in), outbound(&out) {}

    bool available() override { return !inbound->empty(); }
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        std::size_t n = 0;
        while (n < maxLen && !inbound->empty()) {
            buffer[n++] = inbound->front();
            inbound->pop_front();
        }
        return n;
    }
    void write(const std::uint8_t* buffer, std::size_t len) override
    {
        outbound->insert(outbound->end(), buffer, buffer + len);
    }
};

class NoRequests final : public IRequestHandler {
public:
    IOResponse handleRequest(const IORequest& request) override
    {
        return make_base_response(request, StatusCode::InternalError);
    }
};

} // namespace

TEST_CASE("NetIEC ingress: a whole LOAD through the channel and the real device set")
{
    fujinet::fs::StorageManager storage;
    auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    D64Fixture disk;
    Bytes file(600);
    for (std::size_t i = 0; i < file.size(); ++i) file[i] = static_cast<std::uint8_t>(i + 1);
    disk.add_prg(0, "PROGRAM", 0, file);
    mem->file_bytes("/d.d64") = disk.bytes;
    REQUIRE(storage.registerFileSystem(std::move(mem)));
    fujinet::disk::DiskService disks(storage, fujinet::disk::make_default_image_registry());
    REQUIRE(disks.mount(0, "mem", "/d.d64", {}).ok());

    NoRequests requests;
    IecDeviceSet devices(disks, requests);

    std::deque<std::uint8_t> inbound;
    Bytes outbound;
    NetIecIngress ingress(std::make_unique<QueueChannel>(inbound, outbound), devices.bus(), "TEST");

    const auto request = [&](std::uint8_t command, const Bytes& payload) {
        const Bytes wire = frame(command, payload);
        inbound.insert(inbound.end(), wire.begin(), wire.end());
        outbound.clear();
        ingress.poll();
        REQUIRE(outbound.size() >= 3);
        CHECK(outbound[0] == (command | 0x80));
        return Bytes(outbound.begin() + 3, outbound.end());
    };

    // Drives 8 and the NIO channel answer; 9-15 have nothing mounted.
    const Bytes hello = request(0x01, {1});
    const std::uint32_t mask = hello[1] | (hello[2] << 8) | (hello[3] << 16) |
                               (static_cast<std::uint32_t>(hello[4]) << 24);
    CHECK(mask == ((1u << 8) | (1u << 30)));

    CHECK(request(0x02, {0x28, 0xF0}) == Bytes{0});
    for (const char c : std::string("PROGRAM")) {
        CHECK(request(0x03, {0, static_cast<std::uint8_t>(c)}) == Bytes{1, 0});
    }
    CHECK(request(0x02, {0x3F}) == Bytes{0});
    CHECK(request(0x02, {0x48, 0x60}) == Bytes{0});

    Bytes got;
    bool eoi = false;
    while (!eoi) {
        const Bytes reply = request(0x04, {1, 0});
        REQUIRE(reply.size() == 2);
        got.push_back(reply[1]);
        eoi = (reply[0] & 0x01) != 0;
    }
    CHECK(got == file);

    CHECK(request(0x02, {0x5F}) == Bytes{0});
    CHECK(request(0x02, {0x28, 0xE0}) == Bytes{0});
    CHECK(request(0x02, {0x3F}) == Bytes{0});
    CHECK(request(0x02, {0x3F}) == Bytes{0});
}

TEST_CASE("NetIEC ingress: it waits only if its channel can")
{
    IecBus bus;
    std::deque<std::uint8_t> inbound;
    Bytes outbound;
    NetIecIngress ingress(std::make_unique<QueueChannel>(inbound, outbound), bus, "TEST");
    CHECK_FALSE(ingress.supports_work_wait());
    ingress.poll();
    CHECK(outbound.empty());
}
