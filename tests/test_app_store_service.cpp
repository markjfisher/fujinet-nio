#include "doctest.h"

#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/app_store.h"
#include "fujinet/io/devices/app_store_commands.h"
#include "fujinet/io/devices/app_store_service.h"
#include "fake_fs.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fujinet::fs::StorageManager;
using fujinet::io::AppStore;
using fujinet::io::AppStoreService;
using fujinet::io::IORequest;
using fujinet::io::StatusCode;
using fujinet::io::protocol::AppStoreCommand;

constexpr std::uint8_t kVersion = 1;

void append_u8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }

void append_u16le(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void append_u32le(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

std::uint16_t read_u16le(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    return static_cast<std::uint16_t>(data[offset]) |
           (static_cast<std::uint16_t>(data[offset + 1]) << 8);
}

std::uint64_t read_u64le(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[offset + i]) << (8U * i);
    }
    return value;
}

std::string read_len_string(const std::vector<std::uint8_t>& data, std::size_t& offset)
{
    const std::uint16_t len = read_u16le(data, offset);
    offset += 2;
    std::string result(data.begin() + static_cast<std::ptrdiff_t>(offset),
                       data.begin() + static_cast<std::ptrdiff_t>(offset + len));
    offset += len;
    return result;
}

std::vector<std::uint8_t> make_app_store_prefix(std::string_view ns, std::string_view key)
{
    std::vector<std::uint8_t> payload;
    append_u8(payload, kVersion);
    append_u16le(payload, static_cast<std::uint16_t>(ns.size()));
    payload.insert(payload.end(), ns.begin(), ns.end());
    append_u16le(payload, static_cast<std::uint16_t>(key.size()));
    payload.insert(payload.end(), key.begin(), key.end());
    return payload;
}

std::vector<std::uint8_t> make_app_store_stat_request(std::string_view ns, std::string_view key)
{
    return make_app_store_prefix(ns, key);
}

std::vector<std::uint8_t> make_app_store_read_request(
    std::string_view ns, std::string_view key, std::uint32_t offset, std::uint16_t max_bytes)
{
    auto payload = make_app_store_prefix(ns, key);
    append_u32le(payload, offset);
    append_u16le(payload, max_bytes);
    return payload;
}

std::vector<std::uint8_t> make_app_store_write_request(
    std::string_view ns, std::string_view key, std::uint32_t offset, std::string_view data)
{
    auto payload = make_app_store_prefix(ns, key);
    append_u32le(payload, offset);
    append_u16le(payload, static_cast<std::uint16_t>(data.size()));
    payload.insert(payload.end(), data.begin(), data.end());
    return payload;
}

std::vector<std::uint8_t> make_app_store_delete_request(std::string_view ns, std::string_view key)
{
    return make_app_store_prefix(ns, key);
}

std::vector<std::uint8_t> make_app_store_list_request(
    std::string_view ns, std::uint16_t start, std::uint16_t max_payload_bytes)
{
    auto payload = make_app_store_prefix(ns, {});
    append_u16le(payload, start);
    append_u16le(payload, max_payload_bytes);
    return payload;
}

TEST_CASE("AppStoreService write/read/stat stores namespaced values on host filesystem")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));

    AppStoreService device(std::make_shared<AppStore>(storage));

    IORequest write{};
    write.command = static_cast<std::uint16_t>(AppStoreCommand::Write);
    write.payload = make_app_store_write_request("config-ng", "colour.preference", 0, "blue");
    const auto write_response = device.handle(write);
    CHECK(write_response.status == StatusCode::Ok);
    REQUIRE(write_response.payload.size() >= 10);
    CHECK(read_u16le(write_response.payload, 8) == 4);

    IORequest stat{};
    stat.command = static_cast<std::uint16_t>(AppStoreCommand::Stat);
    stat.payload = make_app_store_stat_request("config-ng", "colour.preference");
    const auto stat_response = device.handle(stat);
    CHECK(stat_response.status == StatusCode::Ok);
    REQUIRE(stat_response.payload.size() >= 20);
    CHECK((stat_response.payload[1] & 0x01U) == 0x01U);
    CHECK(read_u64le(stat_response.payload, 4) == 4);

    IORequest read{};
    read.command = static_cast<std::uint16_t>(AppStoreCommand::Read);
    read.payload = make_app_store_read_request("config-ng", "colour.preference", 0, 16);
    const auto read_response = device.handle(read);
    CHECK(read_response.status == StatusCode::Ok);
    REQUIRE(read_response.payload.size() >= 10);
    CHECK((read_response.payload[1] & 0x01U) == 0x01U);
    CHECK((read_response.payload[1] & 0x02U) == 0x02U);
    const auto data_len = read_u16le(read_response.payload, 8);
    REQUIRE(data_len == 4);
    const std::string value(read_response.payload.begin() + 10, read_response.payload.end());
    CHECK(value == "blue");
}

TEST_CASE("AppStoreService supports chunked writes and offset reads")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));

    AppStoreService device(std::make_shared<AppStore>(storage));

    IORequest write{};
    write.command = static_cast<std::uint16_t>(AppStoreCommand::Write);
    write.payload = make_app_store_write_request("app", "state", 0, "hello ");
    CHECK(device.handle(write).status == StatusCode::Ok);

    write.payload = make_app_store_write_request("app", "state", 6, "world");
    CHECK(device.handle(write).status == StatusCode::Ok);

    IORequest read{};
    read.command = static_cast<std::uint16_t>(AppStoreCommand::Read);
    read.payload = make_app_store_read_request("app", "state", 6, 8);
    const auto response = device.handle(read);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= 10);
    CHECK(read_u16le(response.payload, 8) == 5);
    const std::string value(response.payload.begin() + 10, response.payload.end());
    CHECK(value == "world");
}

TEST_CASE("AppStoreService list and delete expose namespace keys")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));

    AppStoreService device(std::make_shared<AppStore>(storage));
    IORequest request{};
    request.command = static_cast<std::uint16_t>(AppStoreCommand::Write);
    request.payload = make_app_store_write_request("prefs", "zeta", 0, "z");
    CHECK(device.handle(request).status == StatusCode::Ok);
    request.payload = make_app_store_write_request("prefs", "alpha", 0, "a");
    CHECK(device.handle(request).status == StatusCode::Ok);

    IORequest list{};
    list.command = static_cast<std::uint16_t>(AppStoreCommand::List);
    list.payload = make_app_store_list_request("prefs", 0, 512);
    const auto list_response = device.handle(list);
    CHECK(list_response.status == StatusCode::Ok);
    REQUIRE(list_response.payload.size() >= 10);
    CHECK((list_response.payload[1] & 0x01U) == 0);
    CHECK(read_u16le(list_response.payload, 6) == 2);

    std::size_t offset = 10;
    const auto first = read_len_string(list_response.payload, offset);
    const auto second = read_len_string(list_response.payload, offset);
    CHECK(first == "alpha");
    CHECK(second == "zeta");

    IORequest del{};
    del.command = static_cast<std::uint16_t>(AppStoreCommand::Delete);
    del.payload = make_app_store_delete_request("prefs", "alpha");
    const auto delete_response = device.handle(del);
    CHECK(delete_response.status == StatusCode::Ok);
    REQUIRE(delete_response.payload.size() >= 4);
    CHECK((delete_response.payload[1] & 0x01U) == 0x01U);

    list.payload = make_app_store_list_request("prefs", 0, 512);
    const auto after_delete = device.handle(list);
    CHECK(after_delete.status == StatusCode::Ok);
    REQUIRE(after_delete.payload.size() >= 10);
    CHECK(read_u16le(after_delete.payload, 6) == 1);
    offset = 10;
    CHECK(read_len_string(after_delete.payload, offset) == "zeta");
}

TEST_CASE("AppStoreService reports missing keys without hard failure")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));

    AppStoreService device(std::make_shared<AppStore>(storage));

    IORequest stat{};
    stat.command = static_cast<std::uint16_t>(AppStoreCommand::Stat);
    stat.payload = make_app_store_stat_request("missing", "key");
    const auto stat_response = device.handle(stat);
    CHECK(stat_response.status == StatusCode::Ok);
    REQUIRE(stat_response.payload.size() >= 20);
    CHECK((stat_response.payload[1] & 0x01U) == 0);
    CHECK(read_u64le(stat_response.payload, 4) == 0);

    IORequest read{};
    read.command = static_cast<std::uint16_t>(AppStoreCommand::Read);
    read.payload = make_app_store_read_request("missing", "key", 0, 16);
    const auto read_response = device.handle(read);
    CHECK(read_response.status == StatusCode::Ok);
    REQUIRE(read_response.payload.size() >= 10);
    CHECK((read_response.payload[1] & 0x01U) == 0x01U);
    CHECK((read_response.payload[1] & 0x02U) == 0);
    CHECK(read_u16le(read_response.payload, 8) == 0);
}

} // namespace
