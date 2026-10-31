#include "doctest.h"

#include "fake_fs.h"
#include "fujinet/core/core.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/atari_sio_ingress_router.h"
#include "fujinet/io/atari_sio_speed.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/virtual_device.h"

#include "../third_party/cjson/cJSON.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

TEST_CASE("advertised HSIO changes preserve the current SIO reply speed")
{
    fujinet::io::AtariSioSpeed speed;
    REQUIRE(speed.set_index(0));
    speed.enter_high();
    fujinet::io::FujiDevice fuji({}, nullptr);
    fuji.set_hsio_handlers([&] { return speed.index(); },
                           [&](int i) { return speed.advertise_index(i); });
    fujinet::io::IORequest req;
    req.command = 3; req.payload = {1, 4};
    auto reply = fuji.handle(req);
    CHECK(reply.status == fujinet::io::StatusCode::Ok);
    CHECK(reply.payload == std::vector<std::uint8_t>{1, 4});
    CHECK(speed.baud() == fujinet::io::AtariSioSpeed::baud_for_index(0));
    req.command = 2; req.payload = {1};
    CHECK(fuji.handle(req).payload == std::vector<std::uint8_t>{1, 4});
    speed.leave_high(); speed.enter_high();
    CHECK(speed.baud() == fujinet::io::AtariSioSpeed::baud_for_index(4));
    req.command = 3; req.payload = {1, 0xff};
    CHECK(fuji.handle(req).status == fujinet::io::StatusCode::Ok);
    CHECK(speed.baud() == fujinet::io::AtariSioSpeed::baud_for_index(4));
    speed.leave_high(); speed.enter_high();
    CHECK(speed.baud() == 19200);
    req.payload = {1, 10};
    CHECK(fuji.handle(req).status == fujinet::io::StatusCode::InvalidRequest);
}

namespace {

using fujinet::io::AtariSioNioFrontend;

std::vector<std::uint8_t> hex_bytes(const char* text)
{
    std::vector<std::uint8_t> out;
    std::istringstream input(text ? text : "");
    unsigned value = 0;
    while (input >> std::hex >> value) out.push_back(static_cast<std::uint8_t>(value));
    return out;
}

struct NioFixture {
    std::array<std::uint8_t, 5> command{};
    std::vector<std::uint8_t> payload;
};

NioFixture nio_fixture(const char* id)
{
    std::ifstream input(std::filesystem::path(FN_ATARI_SIO_FIXTURE_DIR) / "nio_v1.json");
    REQUIRE(input.good());
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    cJSON* root = cJSON_Parse(json.c_str());
    REQUIRE(root != nullptr);
    const cJSON* found = nullptr;
    cJSON_ArrayForEach(found, cJSON_GetObjectItemCaseSensitive(root, "scenarios")) {
        const auto* candidate = cJSON_GetObjectItemCaseSensitive(found, "id");
        if (cJSON_IsString(candidate) && std::string(candidate->valuestring) == id) break;
    }
    REQUIRE(found != nullptr);
    NioFixture out{};
    const auto frame = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "frame")));
    REQUIRE(frame.size() == out.command.size());
    std::copy(frame.begin(), frame.end(), out.command.begin());
    out.payload = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "payload")));
    cJSON_Delete(root);
    return out;
}

std::array<std::uint8_t, 5> command(std::uint8_t device, std::uint8_t op, std::uint16_t aux)
{
    std::array<std::uint8_t, 5> out{device, op, static_cast<std::uint8_t>(aux),
                                    static_cast<std::uint8_t>(aux >> 8), 0};
    out.back() = AtariSioNioFrontend::checksum(out.data(), out.size() - 1);
    return out;
}

std::vector<std::uint8_t> data_frame(const std::vector<std::uint8_t>& payload)
{
    auto result = payload;
    result.push_back(AtariSioNioFrontend::checksum(payload.data(), payload.size()));
    return result;
}

std::vector<std::uint8_t> data_reply(const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> result{'A', 'C'};
    result.insert(result.end(), payload.begin(), payload.end());
    result.push_back(AtariSioNioFrontend::checksum(payload.data(), payload.size()));
    return result;
}

std::vector<std::uint8_t> atr(std::uint8_t value)
{
    std::vector<std::uint8_t> bytes(16 + 128, 0);
    bytes[0] = 0x96;
    bytes[1] = 0x02;
    bytes[2] = 8;
    bytes[4] = 128;
    std::fill(bytes.begin() + 16, bytes.end(), value);
    return bytes;
}

class RecordingDevice final : public fujinet::io::VirtualDevice {
public:
    fujinet::io::IOResponse handle(const fujinet::io::IORequest& request) override
    {
        requests.push_back(request);
        auto response = fujinet::io::make_success_response(request);
        response.payload = {0xaa, 0x55};
        return response;
    }

    std::vector<fujinet::io::IORequest> requests;
};

struct Harness {
    fujinet::fs::StorageManager storage;
    fujinet::disk::DiskService disks{storage, fujinet::disk::make_default_image_registry()};
    fujinet::core::FujinetCore core;
    RecordingDevice* device{new RecordingDevice()};
    fujinet::io::AtariSioDiskFrontend disk_frontend{disks};
    fujinet::io::AtariSioNioFrontend nio_frontend{core.routingManager()};
    std::vector<std::vector<std::uint8_t>> replies;
    fujinet::io::AtariSioIngressRouter router{
        disk_frontend, nio_frontend,
        [this](const std::vector<std::uint8_t>& bytes) { replies.push_back(bytes); }};

    Harness()
    {
        auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
        mem->file_bytes("/d1.atr") = atr(0x11);
        mem->file_bytes("/d8.atr") = atr(0x88);
        REQUIRE(storage.registerFileSystem(std::move(mem)));
        REQUIRE(disks.mount(0, "mem", "/d1.atr", {}).ok());
        REQUIRE(disks.mount(7, "mem", "/d8.atr", {}).ok());
        REQUIRE(core.deviceManager().registerDevice(1, std::unique_ptr<fujinet::io::VirtualDevice>(device)));
    }
};

template <typename Bytes>
void feed(fujinet::io::AtariSioIngressRouter& router, const Bytes& bytes)
{
    for (const auto byte : bytes) router.consume(byte);
}

} // namespace

TEST_CASE("Atari SIO ingress router keeps D1 and D8 on the disk frontend")
{
    Harness harness;

    feed(harness.router, command(0x31, 'R', 1));
    REQUIRE(harness.replies.size() == 1);
    CHECK(harness.replies.back().size() == 2 + 128 + 1);
    CHECK(harness.replies.back()[2] == 0x11);

    feed(harness.router, command(0x38, 'R', 1));
    REQUIRE(harness.replies.size() == 2);
    CHECK(harness.replies.back().size() == 2 + 128 + 1);
    CHECK(harness.replies.back()[2] == 0x88);
    CHECK(harness.device->requests.empty());
}

TEST_CASE("disk read acknowledges a valid command before accessing the image")
{
    Harness h;
    int acknowledgements = 0;
    h.disk_frontend.set_early_read_ack([&] {
        if (acknowledgements == 0) CHECK(h.disks.stats(0).readRequests == 0);
        ++acknowledgements;
    });
    auto bad = command(0x31, 'R', 1);
    bad.back() ^= 1;
    feed(h.router, bad);
    CHECK(acknowledgements == 0);
    REQUIRE(h.replies.back() == std::vector<std::uint8_t>{'N'});
    feed(h.router, command(0x31, 'R', 1));
    CHECK(acknowledgements == 1);
    REQUIRE(h.replies.back().size() == 130);
    CHECK(h.replies.back()[0] == 'C');
    CHECK(h.replies.back()[1] == 0x11);
    feed(h.router, command(0x31, 'R', 0));
    CHECK(acknowledgements == 2);
    CHECK(h.replies.back() == std::vector<std::uint8_t>{'E'});
}

TEST_CASE("Atari SIO ingress router sends native W data to the real core route")
{
    Harness harness;
    const auto write = nio_fixture("w-core-dispatch");
    const auto poll = nio_fixture("p-core-dispatch-ready");
    const auto read = nio_fixture("r-core-dispatch");

    feed(harness.router, command(0x7f, 'Q', 0));
    REQUIRE(harness.replies.back() == data_reply({1, 0, 2, 1}));
    feed(harness.router, write.command);
    REQUIRE(harness.replies.back() == std::vector<std::uint8_t>{'A'});
    feed(harness.router, data_frame(write.payload));
    REQUIRE(harness.replies.back() == std::vector<std::uint8_t>{'A', 'C'});
    REQUIRE(harness.device->requests.size() == 1);

    feed(harness.router, poll.command);
    CHECK(harness.replies.back() == data_reply({2, 9, 0}));

    // A CMD edge between command phases clears byte collection only; it must
    // not discard the ready native response.
    harness.router.reset_command_collection();
    feed(harness.router, read.command);
    const auto terminal = harness.replies.back();
    CHECK(terminal == data_reply(read.payload));
    feed(harness.router, poll.command);
    CHECK(harness.replies.back() == data_reply({0, 0, 0}));
    CHECK(terminal == data_reply(read.payload));
}

TEST_CASE("Atari SIO ingress router rejects malformed or interrupted native data and recovers")
{
    Harness harness;
    const auto write = nio_fixture("w-core-dispatch");
    const auto read = nio_fixture("r-core-dispatch");

    feed(harness.router, write.command);
    const auto partial = data_frame(write.payload);
    feed(harness.router, std::vector<std::uint8_t>(partial.begin(), partial.begin() + 3));
    harness.router.reset_command_collection(); // CMD edge aborts only incomplete W.
    CHECK(harness.device->requests.empty());

    feed(harness.router, write.command);
    feed(harness.router, data_frame(write.payload));
    REQUIRE(harness.device->requests.size() == 1);
    feed(harness.router, read.command);

    feed(harness.router, write.command);
    auto malformed = data_frame(write.payload);
    ++malformed.back();
    feed(harness.router, malformed);
    CHECK(harness.replies.back() == std::vector<std::uint8_t>{'N'});
    CHECK(harness.device->requests.size() == 1);

    feed(harness.router, write.command);
    feed(harness.router, data_frame(write.payload));
    CHECK(harness.replies.back() == std::vector<std::uint8_t>{'A', 'C'});
    CHECK(harness.device->requests.size() == 2);
}

TEST_CASE("Atari SIO ignores unowned IDs and preserves owned errors and recovery")
{
    Harness harness;
    for (const auto id : {0x00, 0x30, 0x39, 0x7e, 0x80, 0xff}) {
        auto frame = command(static_cast<std::uint8_t>(id), 'R', 1);
        feed(harness.router, frame);
        frame.back() ^= 0xff;
        feed(harness.router, frame);
    }
    CHECK(harness.replies.empty());
    feed(harness.router, command(0x31, '?', 0));
    REQUIRE(harness.replies.size() == 1);
    CHECK(harness.replies.back() == std::vector<std::uint8_t>{'N'});
    feed(harness.router, command(0x31, 'R', 1));
    REQUIRE(harness.replies.size() == 2);
    CHECK(harness.replies.back().size() == 131);
}

TEST_CASE("Atari SIO ingress router counts commands by owner and remembers the last frame")
{
    Harness harness;
    CHECK_FALSE(harness.router.stats().has_last_frame);

    feed(harness.router, command(0x31, 'S', 0));   // D1: status
    feed(harness.router, command(0x7f, 'Q', 0));   // native query
    feed(harness.router, command(0x45, 'S', 0));   // another bus device: ignored on purpose
    feed(harness.router, command(0x45, 'R', 1));

    const auto& stats = harness.router.stats();
    CHECK(stats.disk_commands == 1);
    CHECK(stats.nio_commands == 1);
    CHECK(stats.other_commands == 2);
    REQUIRE(stats.has_last_frame);
    CHECK(stats.last_frame[0] == 0x45);
    CHECK(stats.last_frame[1] == 'R');
    CHECK(stats.last_frame == command(0x45, 'R', 1));

    harness.router.reset_stats();
    CHECK(harness.router.stats().disk_commands == 0);
    CHECK(harness.router.stats().other_commands == 0);
    CHECK_FALSE(harness.router.stats().has_last_frame);
}

TEST_CASE("Atari SIO answers the high-speed index query only when high-speed SIO is on")
{
    Harness harness;
    int switches = 0;
    std::size_t replies_when_switched = 0;
    harness.router.set_high_speed_handler([&] { ++switches; replies_when_switched = harness.replies.size(); });

    // Off (the default): NAK, no switch.
    feed(harness.router, command(0x31, '?', 0x20));
    CHECK(harness.replies.back() == std::vector<std::uint8_t>{'N'});
    CHECK(switches == 0);

    // On: ACK, COMPLETE, the index and its checksum, then the switch, after the reply was sent.
    harness.disk_frontend.set_hsio_index(6);
    feed(harness.router, command(0x31, '?', 0x20));   // drivers send AUX1 $20; AUX is ignored
    CHECK(harness.replies.back() == std::vector<std::uint8_t>{'A', 'C', 6, 6});
    CHECK(switches == 1);
    CHECK(replies_when_switched == harness.replies.size());
    feed(harness.router, command(0x38, '?', 0));
    CHECK(harness.replies.back() == std::vector<std::uint8_t>{'A', 'C', 6, 6});
    CHECK(switches == 2);

    // Ordinary commands never switch.
    feed(harness.router, command(0x31, 'R', 1));
    CHECK(switches == 2);

    harness.disk_frontend.set_hsio_index(-1);
    feed(harness.router, command(0x31, '?', 0));
    CHECK(harness.replies.back() == std::vector<std::uint8_t>{'N'});
    CHECK(switches == 2);
}

TEST_CASE("Atari SIO native endpoint answers the high-speed index query too, without disturbing a transaction")
{
    Harness harness;
    int switches = 0;
    harness.router.set_high_speed_handler([&] { ++switches; });

    feed(harness.router, command(0x7f, '?', 0x20));          // off: NAK, counted as refused
    CHECK(harness.replies.back() == std::vector<std::uint8_t>{'N'});
    CHECK(switches == 0);
    CHECK(harness.router.stats().refused == 1);

    harness.nio_frontend.set_hsio_index(6);
    feed(harness.router, command(0x7f, 'Q', 0));             // a query leaves the endpoint idle
    feed(harness.router, command(0x7f, '?', 0x20));
    CHECK(harness.replies.back() == std::vector<std::uint8_t>{'A', 'C', 6, 6});
    CHECK(switches == 1);
    CHECK(harness.router.stats().hsio_queries_nio == 2);
    CHECK(harness.router.stats().hsio_queries_disk == 0);
    feed(harness.router, command(0x7f, 'Q', 0));             // still works afterwards
    CHECK(harness.replies.back().size() == 7);
}

TEST_CASE("Atari SIO router remembers the first and latest refused frame and the latest other-device frame")
{
    Harness harness;
    CHECK(harness.router.stats().refused == 0);
    CHECK_FALSE(harness.router.stats().has_last_other);

    feed(harness.router, command(0x31, 'S', 0));              // answered: not refused
    feed(harness.router, command(0x31, 0x80 | 'R', 1));       // a command the disk does not have
    feed(harness.router, command(0x7f, 'X', 0));
    feed(harness.router, command(0x70, 0x3f, 0x20));          // another device

    const auto& stats = harness.router.stats();
    CHECK(stats.refused == 2);
    CHECK(stats.first_refused == command(0x31, 0x80 | 'R', 1));
    CHECK(stats.last_refused == command(0x7f, 'X', 0));
    REQUIRE(stats.has_last_other);
    CHECK(stats.last_other == command(0x70, 0x3f, 0x20));
    REQUIRE(stats.other_first_count == 1);
    CHECK(stats.other_first[0] == command(0x70, 0x3f, 0x20));

    for (int i = 0; i < 12; ++i) feed(harness.router, command(0x71, 'S', 0));
    CHECK(harness.router.stats().other_first_count == 8);           // only the first eight are kept
    CHECK(harness.router.stats().other_first[0] == command(0x70, 0x3f, 0x20));
    CHECK(harness.router.stats().other_first[7] == command(0x71, 'S', 0));
}

TEST_CASE("Atari SIO speed state: legacy baud table, range, and falling back")
{
    using fujinet::io::AtariSioSpeed;
    CHECK(AtariSioSpeed::baud_for_index(0) == 124017);
    CHECK(AtariSioSpeed::baud_for_index(6) == 67431);
    CHECK(AtariSioSpeed::baud_for_index(9) == 54905);

    AtariSioSpeed speed;
    CHECK_FALSE(speed.enabled());
    speed.enter_high();                       // off: nothing happens
    CHECK_FALSE(speed.high());
    CHECK(speed.baud() == 19200);

    CHECK_FALSE(speed.set_index(10));         // 10 would be read as a Happy 1050
    CHECK_FALSE(speed.set_index(-2));
    CHECK(speed.set_index(6));
    CHECK(speed.baud() == 19200);             // enabled, but the Atari has not asked yet
    speed.enter_high();
    CHECK(speed.baud() == 67431);
    speed.leave_high();
    CHECK(speed.baud() == 19200);
    speed.enter_high();
    CHECK(speed.set_index(-1));               // turning it off drops back to standard
    CHECK_FALSE(speed.high());
    CHECK(speed.baud() == 19200);
}
