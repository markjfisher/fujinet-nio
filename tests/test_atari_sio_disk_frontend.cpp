#include "doctest.h"

#include "fake_fs.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/atari_sio_disk_frontend.h"
#include "../third_party/cjson/cJSON.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace {
std::vector<std::uint8_t> classic256_atr()
{
    // Three 128-byte boot sectors and one 256-byte sector, the exact layout
    // selected by the canonical fixture's sector-4 boundary scenario.
    std::vector<std::uint8_t> image(16 + 384 + 256, 0);
    const unsigned paragraphs = (384 + 256) / 16;
    image[0] = 0x96; image[1] = 0x02;
    image[2] = paragraphs & 0xff; image[3] = paragraphs >> 8;
    image[4] = 0x00; image[5] = 0x01;
    for (std::size_t i = 0; i < 128; ++i) image[16 + i] = static_cast<std::uint8_t>(i);
    for (std::size_t i = 0; i < 256; ++i) image[16 + 384 + i] = static_cast<std::uint8_t>(0xa0 + i);
    return image;
}

std::array<std::uint8_t, 5> command(std::uint8_t device, std::uint8_t op, std::uint16_t sector)
{
    std::array<std::uint8_t, 5> value{device, op, static_cast<std::uint8_t>(sector), static_cast<std::uint8_t>(sector >> 8), 0};
    value.back() = fujinet::io::AtariSioDiskFrontend::checksum(value.data(), 4);
    return value;
}

struct FixtureScenario {
    std::string id;
    std::vector<std::uint8_t> command;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> writeData;
    std::vector<std::uint8_t> status;
};

std::vector<std::uint8_t> hex_bytes(const char* text)
{
    std::vector<std::uint8_t> out;
    if (!text) return out;
    std::istringstream input(text);
    unsigned value = 0;
    while (input >> std::hex >> value) out.push_back(static_cast<std::uint8_t>(value));
    return out;
}

FixtureScenario fixture(const char* id)
{
    const auto path = std::filesystem::path(FN_ATARI_SIO_FIXTURE_DIR) / "disk_v1.json";
    std::ifstream input(path);
    REQUIRE_MESSAGE(input.good(), "fixture path: " << path);
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    cJSON* root = cJSON_Parse(json.c_str());
    REQUIRE(root != nullptr);
    const cJSON* scenarios = cJSON_GetObjectItemCaseSensitive(root, "scenarios");
    const cJSON* found = nullptr;
    cJSON_ArrayForEach(found, scenarios) {
        const cJSON* candidate = cJSON_GetObjectItemCaseSensitive(found, "id");
        if (cJSON_IsString(candidate) && std::string(candidate->valuestring) == id) break;
    }
    REQUIRE(found != nullptr);
    FixtureScenario result{};
    result.id = id;
    result.command = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "command")));
    result.data = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "data")));
    result.writeData = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "write_data")));
    result.status = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "status_after")));
    cJSON_Delete(root);
    return result;
}

std::array<std::uint8_t, 5> fixture_command(const FixtureScenario& scenario)
{
    REQUIRE(scenario.command.size() == 5);
    std::array<std::uint8_t, 5> out{};
    std::copy_n(scenario.command.begin(), out.size(), out.begin());
    return out;
}

std::vector<std::uint8_t> fixture_atr(const std::vector<FixtureScenario>& sectors)
{
    std::vector<std::uint8_t> image(16 + 896, 0); // canonical classic-256 geometry: five sectors
    image[0] = 0x96; image[1] = 0x02; image[2] = 56; image[4] = 0; image[5] = 1;
    for (const auto& sector : sectors) {
        const unsigned number = sector.command[2] | (sector.command[3] << 8);
        const std::size_t offset = 16 + (number <= 3 ? (number - 1) * 128 : 384 + (number - 4) * 256);
        std::copy(sector.data.begin(), sector.data.end(), image.begin() + offset);
    }
    return image;
}

std::vector<std::uint8_t> fixture_128_atr()
{
    std::vector<std::uint8_t> image(16 + 128, 0);
    image[0] = 0x96; image[1] = 0x02; image[2] = 8; image[4] = 128;
    return image;
}
}

TEST_CASE("Atari SIO disk frontend follows fixture command phases and classic-256 sizes")
{
    fujinet::fs::StorageManager storage;
    auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    mem->file_bytes("/disk.atr") = classic256_atr();
    REQUIRE(storage.registerFileSystem(std::move(mem)));
    fujinet::disk::DiskService disks(storage, fujinet::disk::make_default_image_registry());
    REQUIRE(disks.mount(0, "mem", "/disk.atr", {}).ok());
    fujinet::io::AtariSioDiskFrontend frontend(disks);

    auto boot = frontend.begin(command(0x31, 0x52, 1));
    REQUIRE(boot.bytes[0] == 0x41);
    REQUIRE(boot.bytes[1] == 0x43);
    REQUIRE(boot.bytes.size() == 2 + 128 + 1);
    auto sector4 = frontend.begin(command(0x31, 0x52, 4));
    REQUIRE(sector4.bytes.size() == 2 + 256 + 1);

    auto invalid = frontend.begin(command(0x31, 0x52, 0));
    REQUIRE(invalid.bytes == std::vector<std::uint8_t>{0x41, 0x45});
    auto status = frontend.begin(command(0x31, 0x53, 0));
    for (const auto aux : {0x0028, 0x0015, 0xffff}) {
        REQUIRE(frontend.begin(command(0x31, 0x53, aux)).bytes == status.bytes);
    }
    REQUIRE(status.bytes == std::vector<std::uint8_t>{0x41, 0x43, 0x24, 0xcf, 0x0f, 0x00,
        fujinet::io::AtariSioDiskFrontend::checksum(status.bytes.data() + 2, 4)});
}

TEST_CASE("Atari SIO disk frontend rejects bad command and write checksums without mutation")
{
    fujinet::fs::StorageManager storage;
    auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    mem->file_bytes("/disk.atr") = classic256_atr();
    REQUIRE(storage.registerFileSystem(std::move(mem)));
    fujinet::disk::DiskService disks(storage, fujinet::disk::make_default_image_registry());
    REQUIRE(disks.mount(0, "mem", "/disk.atr", {}).ok());
    fujinet::io::AtariSioDiskFrontend frontend(disks);
    auto malformed = command(0x31, 0x52, 1); ++malformed[4];
    REQUIRE(frontend.begin(malformed).bytes == std::vector<std::uint8_t>{0x4e});
    auto put = frontend.begin(command(0x31, 0x50, 4));
    REQUIRE(put.expected_write_bytes == 256);
    std::vector<std::uint8_t> data(257, 0x55);
    data.back() = 0; // deliberately wrong checksum
    REQUIRE(frontend.complete_write(data.data(), data.size()).bytes == std::vector<std::uint8_t>{0x4e});
    auto read = frontend.begin(command(0x31, 0x52, 4));
    REQUIRE(read.bytes[2] == 0xa0);
}

TEST_CASE("Atari SIO disk frontend lazily mounts data operations but not STATUS")
{
    fujinet::fs::StorageManager storage;
    auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    mem->file_bytes("/disk.atr") = classic256_atr();
    REQUIRE(storage.registerFileSystem(std::move(mem)));
    fujinet::disk::DiskService disks(storage, fujinet::disk::make_default_image_registry());
    disks.set_pending_mount(0, "mem:/disk.atr", "rw", true);
    fujinet::io::AtariSioDiskFrontend frontend(disks);

    const auto inspection = frontend.begin(command(0x31, 0x53, 0));
    CHECK_FALSE(disks.info(0).inserted);
    CHECK(disks.get_pending_mount(0).has_value());
    CHECK(inspection.bytes[1] == 0x43);

    const auto accepted = frontend.begin(command(0x31, 0x57, 4));
    REQUIRE(accepted.bytes == std::vector<std::uint8_t>{0x41});
    CHECK(accepted.expected_write_bytes == 256);
    CHECK(disks.info(0).inserted);
    CHECK_FALSE(disks.get_pending_mount(0).has_value());
}

TEST_CASE("Atari SIO disk frontend consumes canonical disk-v1 fixture scenarios")
{
    const auto boot1 = fixture("boot-config-read-sector-1");
    const auto boot2 = fixture("boot-config-read-sector-2");
    const auto boot3 = fixture("boot-config-read-sector-3");
    const auto boot4 = fixture("boot-config-read-sector-4");
    const auto write = fixture("normal-restored-write-verify");
    const auto readback = fixture("normal-restored-write-verify-readback");
    const auto invalid = fixture("zero-sector-execution-failure");
    const auto retained = fixture("status-retains-error");

    fujinet::fs::StorageManager storage;
    auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    mem->file_bytes("/boot.atr") = fixture_atr({boot1, boot2, boot3, boot4});
    mem->file_bytes("/write.atr") = fixture_atr({});
    mem->file_bytes("/plain.atr") = fixture_128_atr();
    REQUIRE(storage.registerFileSystem(std::move(mem)));
    fujinet::disk::DiskService disks(storage, fujinet::disk::make_default_image_registry());
    REQUIRE(disks.mount(0, "mem", "/boot.atr", {}).ok());
    REQUIRE(disks.mount(2, "mem", "/write.atr", {}).ok());
    fujinet::io::AtariSioDiskFrontend frontend(disks);

    for (const auto& scenario : {boot1, boot2, boot3, boot4}) {
        const auto reply = frontend.begin(fixture_command(scenario));
        REQUIRE(reply.bytes[0] == 0x41);
        REQUIRE(reply.bytes[1] == 0x43);
        REQUIRE(std::vector<std::uint8_t>(reply.bytes.begin() + 2, reply.bytes.end() - 1) == scenario.data);
    }
    const auto accepted = frontend.begin(fixture_command(write));
    REQUIRE(accepted.expected_write_bytes == write.writeData.size());
    auto writeFrame = write.writeData;
    writeFrame.push_back(fujinet::io::AtariSioDiskFrontend::checksum(writeFrame.data(), writeFrame.size()));
    REQUIRE(frontend.complete_write(writeFrame.data(), writeFrame.size()).bytes == std::vector<std::uint8_t>{0x41, 0x43});
    const auto read = frontend.begin(fixture_command(readback));
    REQUIRE(std::vector<std::uint8_t>(read.bytes.begin() + 2, read.bytes.end() - 1) == readback.data);

    REQUIRE(disks.unmount(0).ok());
    REQUIRE(disks.mount(0, "mem", "/plain.atr", {}).ok());
    REQUIRE(frontend.begin(fixture_command(invalid)).bytes == std::vector<std::uint8_t>{0x41, 0x45});
    const auto status = frontend.begin(fixture_command(retained));
    REQUIRE(std::vector<std::uint8_t>(status.bytes.begin() + 2, status.bytes.begin() + 6) == retained.status);
}
