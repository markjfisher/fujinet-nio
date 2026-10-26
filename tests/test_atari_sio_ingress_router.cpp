#include "doctest.h"

#include "fake_fs.h"
#include "fujinet/core/core.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/atari_sio_ingress_router.h"
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

TEST_CASE("Atari SIO ingress router sends native W data to the real core route")
{
    Harness harness;
    const auto write = nio_fixture("w-core-dispatch");
    const auto poll = nio_fixture("p-core-dispatch-ready");
    const auto read = nio_fixture("r-core-dispatch");

    feed(harness.router, command(0x7f, 'Q', 0));
    REQUIRE(harness.replies.back() == data_reply({1, 0, 2, 0}));
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
