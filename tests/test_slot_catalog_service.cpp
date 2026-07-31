#include "doctest.h"

#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/app_store.h"
#include "fujinet/io/devices/slot_catalog_commands.h"
#include "fujinet/io/devices/slot_catalog_service.h"
#include "fake_fs.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fujinet::fs::StorageManager;
using fujinet::io::AppStore;
using fujinet::io::IORequest;
using fujinet::io::SlotCatalogService;
using fujinet::io::StatusCode;
using fujinet::io::protocol::SlotCatalogCommand;

constexpr std::uint8_t kVersion = 1;

void append_u8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }

void append_u16le(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

std::uint16_t read_u16le(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    return static_cast<std::uint16_t>(data[offset]) |
           (static_cast<std::uint16_t>(data[offset + 1]) << 8);
}

std::vector<std::uint8_t> make_slot_catalog_range_request(
    std::uint8_t lower,
    std::uint8_t upper,
    std::uint8_t cursor,
    std::uint8_t flags,
    std::uint8_t max_uri_bytes,
    std::uint16_t max_payload_bytes)
{
    std::vector<std::uint8_t> payload;
    append_u8(payload, kVersion);
    append_u8(payload, lower);
    append_u8(payload, upper);
    append_u8(payload, cursor);
    append_u8(payload, flags);
    append_u8(payload, max_uri_bytes);
    append_u16le(payload, max_payload_bytes);
    return payload;
}

std::vector<std::uint8_t> make_slot_catalog_get_request(std::uint8_t index)
{
    return {kVersion, index};
}

std::vector<std::uint8_t> make_slot_catalog_put_request(
    std::uint8_t index, std::uint8_t flags, std::string_view target)
{
    std::vector<std::uint8_t> payload{kVersion, index, flags};
    append_u16le(payload, static_cast<std::uint16_t>(target.size()));
    payload.insert(payload.end(), target.begin(), target.end());
    return payload;
}

std::vector<std::uint8_t> make_slot_catalog_delete_request(std::uint8_t index)
{
    return {kVersion, index};
}

TEST_CASE("SlotCatalogService owns canonical put get and delete operations")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(
        std::make_unique<fujinet::tests::MemoryFileSystem>("host")));
    auto store = std::make_shared<AppStore>(storage);
    AppStore::WriteResult currentWrite{};
    const std::string currentHost = "host:/games";
    CHECK(store->write(
        "fujinet-nio", "current-host", 0,
        reinterpret_cast<const std::uint8_t*>(currentHost.data()),
        static_cast<std::uint16_t>(currentHost.size()), currentWrite));

    SlotCatalogService device(storage, store);
    IORequest put{};
    put.command = static_cast<std::uint16_t>(SlotCatalogCommand::Put);
    put.payload = make_slot_catalog_put_request(100, 0, "elite.ssd");
    const auto putResponse = device.handle(put);
    REQUIRE(putResponse.status == StatusCode::Ok);
    REQUIRE(putResponse.payload.size() >= 5);
    CHECK(putResponse.payload[1] ==
          fujinet::io::protocol::slot_catalog::kEntryValid);
    CHECK(putResponse.payload[2] == 100);
    CHECK(std::string(putResponse.payload.begin() + 5,
                      putResponse.payload.end()) == "host:/games/elite.ssd");

    IORequest get{};
    get.command = static_cast<std::uint16_t>(SlotCatalogCommand::Get);
    get.payload = make_slot_catalog_get_request(100);
    const auto getResponse = device.handle(get);
    CHECK(getResponse.status == StatusCode::Ok);
    CHECK(getResponse.payload == putResponse.payload);

    AppStore::ReadResult stored{};
    REQUIRE(store->read("config-nio", "slot-100", 0, 128, stored));
    REQUIRE(stored.exists);
    CHECK(std::string(stored.data.begin(), stored.data.end()) ==
          std::string{"\x01\x00host:/games/elite.ssd", 23});

    IORequest del{};
    del.command = static_cast<std::uint16_t>(SlotCatalogCommand::Delete);
    del.payload = make_slot_catalog_delete_request(100);
    const auto deleteResponse = device.handle(del);
    REQUIRE(deleteResponse.status == StatusCode::Ok);
    REQUIRE(deleteResponse.payload.size() == 3);
    CHECK(deleteResponse.payload[1] ==
          fujinet::io::protocol::slot_catalog::kDeleteRemoved);
    CHECK(device.handle(get).status == StatusCode::DeviceNotFound);
}

TEST_CASE("SlotCatalogService returns sparse ranges and URI tails")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));
    auto store = std::make_shared<AppStore>(storage);
    SlotCatalogService device(storage, store);

    IORequest write{};
    write.command = static_cast<std::uint16_t>(SlotCatalogCommand::Put);
    write.payload = make_slot_catalog_put_request(
        20, fujinet::io::protocol::slot_catalog::kEntryReadOnly,
        "tnfs://server/archive/games/elite.ssd");
    CHECK(device.handle(write).status == StatusCode::Ok);
    write.payload = make_slot_catalog_put_request(22, 0, "host:/dev.ssd");
    CHECK(device.handle(write).status == StatusCode::Ok);

    IORequest range{};
    range.command = static_cast<std::uint16_t>(SlotCatalogCommand::Range);
    range.payload = make_slot_catalog_range_request(
        16, 23, 16, fujinet::io::protocol::slot_catalog::kRequestTailUri, 12, 128);
    const auto response = device.handle(range);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= 8);
    CHECK((response.payload[1] & fujinet::io::protocol::slot_catalog::kResponseMore) == 0);
    CHECK(response.payload[3] == 1);
    CHECK(response.payload[4] == 2);
    CHECK(response.payload[7] == 0x50);

    std::size_t offset = 8;
    CHECK(response.payload[offset++] == 20);
    CHECK((response.payload[offset] &
           fujinet::io::protocol::slot_catalog::kEntryValid) != 0);
    CHECK((response.payload[offset] &
           fujinet::io::protocol::slot_catalog::kEntryReadOnly) != 0);
    CHECK((response.payload[offset++] &
           fujinet::io::protocol::slot_catalog::kEntryUriTruncated) != 0);
    const auto uri_len = response.payload[offset++];
    CHECK(std::string(response.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                      response.payload.begin() + static_cast<std::ptrdiff_t>(offset + uri_len)) ==
          "es/elite.ssd");
}

TEST_CASE("SlotCatalogService cache follows writes and deletes")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));
    auto store = std::make_shared<AppStore>(storage);
    SlotCatalogService device(storage, store);

    IORequest range{};
    range.command = static_cast<std::uint16_t>(SlotCatalogCommand::Range);
    range.payload = make_slot_catalog_range_request(64, 71, 64, 0, 128, 220);
    auto response = device.handle(range);
    REQUIRE(response.payload.size() >= 8);
    CHECK(response.payload[7] == 0);

    IORequest write{};
    write.command = static_cast<std::uint16_t>(SlotCatalogCommand::Put);
    write.payload = make_slot_catalog_put_request(69, 0, "host:/elite.ssd");
    CHECK(device.handle(write).status == StatusCode::Ok);

    response = device.handle(range);
    REQUIRE(response.payload.size() >= 8);
    CHECK(response.payload[7] == 0x20);
    CHECK(response.payload[4] == 1);

    IORequest del{};
    del.command = static_cast<std::uint16_t>(SlotCatalogCommand::Delete);
    del.payload = make_slot_catalog_delete_request(69);
    CHECK(device.handle(del).status == StatusCode::Ok);
    response = device.handle(range);
    REQUIRE(response.payload.size() >= 8);
    CHECK(response.payload[7] == 0);
    CHECK(response.payload[4] == 0);
}

TEST_CASE("SlotCatalogService continues at a complete record")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));
    auto store = std::make_shared<AppStore>(storage);
    SlotCatalogService device(storage, store);

    IORequest write{};
    write.command = static_cast<std::uint16_t>(SlotCatalogCommand::Put);
    for (const auto index : {0, 1, 2}) {
        write.payload = make_slot_catalog_put_request(
            static_cast<std::uint8_t>(index), 0, "host:/disk.ssd");
        CHECK(device.handle(write).status == StatusCode::Ok);
    }

    IORequest range{};
    range.command = static_cast<std::uint16_t>(SlotCatalogCommand::Range);
    range.payload = make_slot_catalog_range_request(0, 7, 0, 0, 128, 22);
    const auto first = device.handle(range);
    REQUIRE(first.payload.size() >= 8);
    CHECK((first.payload[1] &
           fujinet::io::protocol::slot_catalog::kResponseMore) != 0);
    CHECK(first.payload[2] == 1);
    CHECK(first.payload[4] == 1);

    range.payload = make_slot_catalog_range_request(0, 7, first.payload[2], 0, 128, 64);
    const auto second = device.handle(range);
    REQUIRE(second.payload.size() >= 8);
    CHECK((second.payload[1] &
           fujinet::io::protocol::slot_catalog::kResponseMore) == 0);
    CHECK(second.payload[4] == 2);
}

TEST_CASE("SlotCatalogService formats populated entries for CLI clients")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));
    auto store = std::make_shared<AppStore>(storage);
    SlotCatalogService device(storage, store);

    IORequest write{};
    write.command = static_cast<std::uint16_t>(SlotCatalogCommand::Put);
    write.payload = make_slot_catalog_put_request(69, 0, "host:/games/elite.ssd");
    CHECK(device.handle(write).status == StatusCode::Ok);

    IORequest range{};
    range.command = static_cast<std::uint16_t>(SlotCatalogCommand::Range);
    range.payload = make_slot_catalog_range_request(
        64, 72, 64, fujinet::io::protocol::slot_catalog::kRequestFormatted, 128, 220);
    const auto response = device.handle(range);
    REQUIRE(response.payload.size() >= 9);
    CHECK((response.payload[1] &
           fujinet::io::protocol::slot_catalog::kResponseFormatted) != 0);
    const std::size_t text_offset = 7 + response.payload[3];
    CHECK(std::string(response.payload.begin() + static_cast<std::ptrdiff_t>(text_offset),
                      response.payload.end()) == "69: host:/games/elite.ssd\n");
}

} // namespace
