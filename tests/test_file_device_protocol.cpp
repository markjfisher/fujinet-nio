#include "doctest.h"

#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/app_store.h"
#include "fujinet/io/devices/file_commands.h"
#include "fujinet/io/devices/file_device.h"
#include "fujinet/io/devices/host_commands.h"
#include "fujinet/io/devices/host_service.h"
#include "fake_fs.h"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fujinet::fs::FileInfo;
using fujinet::fs::FileSystemKind;
using fujinet::fs::IFile;
using fujinet::fs::IFileSystem;
using fujinet::fs::StorageManager;
using fujinet::io::AppStore;
using fujinet::io::FileDevice;
using fujinet::io::HostService;
using fujinet::io::IORequest;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FileCommand;
using fujinet::io::protocol::HostCommand;
using fujinet::io::protocol::list_directory::kListFlagCompactOmitMetadata;
using fujinet::io::protocol::list_directory::kListFlagFormattedLines;
using fujinet::io::protocol::list_directory::kListFlagSortByName;
using fujinet::io::protocol::list_directory::kListEntryFlagNameTruncated;
using fujinet::io::protocol::list_directory::kListResponseFlagNameTruncated;

constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kListDirHeaderBytes = 10;
constexpr std::uint16_t kListMaxPayloadBytes = 512;

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

std::vector<std::uint8_t> make_uri_request(std::string_view uri)
{
    std::vector<std::uint8_t> payload;
    append_u8(payload, kVersion);
    append_u16le(payload, static_cast<std::uint16_t>(uri.size()));
    payload.insert(payload.end(), uri.begin(), uri.end());
    return payload;
}

std::vector<std::uint8_t> make_list_request(
    std::string_view uri, std::uint16_t start, std::uint16_t max_payload_bytes)
{
    auto payload = make_uri_request(uri);
    append_u16le(payload, start);
    append_u16le(payload, max_payload_bytes);
    return payload;
}

std::vector<std::uint8_t> make_list_request_with_flags(
    std::string_view uri,
    std::uint16_t start,
    std::uint16_t max_payload_bytes,
    std::uint8_t list_flags)
{
    auto payload = make_list_request(uri, start, max_payload_bytes);
    append_u8(payload, list_flags);
    return payload;
}

std::vector<std::uint8_t> make_list_request_with_limits(
    std::string_view uri,
    std::uint16_t start,
    std::uint16_t max_payload_bytes,
    std::uint8_t list_flags,
    std::uint8_t max_name_bytes)
{
    auto payload = make_list_request_with_flags(uri, start, max_payload_bytes, list_flags);
    append_u8(payload, max_name_bytes);
    return payload;
}

std::vector<std::uint8_t> make_read_request(
    std::string_view uri, std::uint32_t offset, std::uint16_t max_bytes)
{
    auto payload = make_uri_request(uri);
    append_u32le(payload, offset);
    append_u16le(payload, max_bytes);
    return payload;
}

std::vector<std::uint8_t> make_write_request(
    std::string_view uri, std::uint32_t offset, std::string_view data)
{
    auto payload = make_uri_request(uri);
    append_u32le(payload, offset);
    append_u16le(payload, static_cast<std::uint16_t>(data.size()));
    payload.insert(payload.end(), data.begin(), data.end());
    return payload;
}

std::vector<std::uint8_t> make_mkdir_request(
    std::string_view uri, bool parents, bool exist_ok)
{
    auto payload = make_uri_request(uri);
    std::uint8_t flags = 0;
    if (parents) flags |= 0x01;
    if (exist_ok) flags |= 0x02;
    append_u8(payload, flags);
    return payload;
}

std::vector<std::uint8_t> make_resolve_path_request(std::string_view base_uri, std::string_view arg)
{
    std::vector<std::uint8_t> payload;
    append_u8(payload, kVersion);
    append_u16le(payload, static_cast<std::uint16_t>(base_uri.size()));
    payload.insert(payload.end(), base_uri.begin(), base_uri.end());
    append_u16le(payload, static_cast<std::uint16_t>(arg.size()));
    payload.insert(payload.end(), arg.begin(), arg.end());
    return payload;
}

std::size_t list_entry_span_bytes(std::uint8_t name_len, bool compact)
{
    return 2U + static_cast<std::size_t>(name_len) + (compact ? 0U : 16U);
}

std::string_view first_list_entry_name(const std::vector<std::uint8_t>& payload)
{
    REQUIRE(payload.size() >= kListDirHeaderBytes + 2);
    const std::uint8_t name_len = payload[kListDirHeaderBytes + 1];
    REQUIRE(payload.size() >= kListDirHeaderBytes + 2U + static_cast<std::size_t>(name_len));
    return std::string_view{
        reinterpret_cast<const char*>(payload.data() + kListDirHeaderBytes + 2),
        name_len};
}

std::vector<std::uint8_t> make_host_set_request(std::string_view spec)
{
    std::vector<std::uint8_t> payload;
    append_u8(payload, kVersion);
    append_u16le(payload, static_cast<std::uint16_t>(spec.size()));
    payload.insert(payload.end(), spec.begin(), spec.end());
    return payload;
}

std::vector<std::uint8_t> make_host_list_request(std::uint16_t offset, std::uint16_t max_bytes)
{
    std::vector<std::uint8_t> payload;
    append_u8(payload, kVersion);
    append_u16le(payload, offset);
    append_u16le(payload, max_bytes);
    return payload;
}

std::vector<std::uint8_t> make_host_index_request(std::uint8_t index)
{
    return {kVersion, index};
}

std::string host_history_text(HostService& device)
{
    IORequest list{};
    list.command = static_cast<std::uint16_t>(HostCommand::ListHistory);
    list.payload = make_host_list_request(0, 512);
    const auto response = device.handle(list);
    CHECK(response.status == StatusCode::Ok);
    if (response.status != StatusCode::Ok || response.payload.size() < 6) {
        return {};
    }
    const auto len = read_u16le(response.payload, 4);
    return std::string(response.payload.begin() + 6, response.payload.begin() + 6 + len);
}

std::string host_current_uri(HostService& device)
{
    IORequest get{};
    get.command = static_cast<std::uint16_t>(HostCommand::GetCurrent);
    get.payload = {kVersion};
    const auto response = device.handle(get);
    CHECK(response.status == StatusCode::Ok);
    if (response.status != StatusCode::Ok || response.payload.size() < 5) {
        return {};
    }
    const auto len = read_u16le(response.payload, 1);
    return std::string(response.payload.begin() + 5, response.payload.begin() + 5 + len);
}

class NullFile final : public IFile {
public:
    std::size_t read(void*, std::size_t) override { return 0; }
    std::size_t write(const void*, std::size_t bytes) override { return bytes; }
    bool seek(std::uint64_t) override { return true; }
    std::uint64_t tell() const override { return 0; }
    bool flush() override { return true; }
};

class MemoryFs final : public IFileSystem {
public:
    explicit MemoryFs(std::string name) : _name(std::move(name)) {}

    FileSystemKind kind() const override { return FileSystemKind::HostPosix; }
    std::string name() const override { return _name; }
    bool exists(const std::string& path) override { return _entries.count(path) > 0; }
    bool isDirectory(const std::string& path) override
    {
        auto it = _entries.find(path);
        return it != _entries.end() && it->second.isDirectory;
    }
    bool createDirectory(const std::string& path) override
    {
        _entries[path] = FileInfo{path, true, 0, {}};
        return true;
    }
    bool removeFile(const std::string&) override { return false; }
    bool removeDirectory(const std::string&) override { return false; }
    bool rename(const std::string&, const std::string&) override { return false; }
    std::unique_ptr<IFile> open(const std::string&, const char*) override { return std::make_unique<NullFile>(); }
    bool stat(const std::string& path, FileInfo& outInfo) override
    {
        auto it = _entries.find(path);
        if (it == _entries.end()) return false;
        outInfo = it->second;
        return true;
    }
    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override
    {
        auto it = _directories.find(path);
        if (it == _directories.end()) return false;
        outEntries = it->second;
        return true;
    }

    void add_entry(const std::string& path, bool is_dir, std::uint64_t size = 0)
    {
        _entries[path] = FileInfo{path, is_dir, size, {}};
    }

    void set_directory(const std::string& path, std::vector<FileInfo> entries)
    {
        _entries[path] = FileInfo{path, true, 0, {}};
        _directories[path] = std::move(entries);
    }

private:
    std::string _name;
    std::map<std::string, FileInfo> _entries;
    std::map<std::string, std::vector<FileInfo>> _directories;
};

/** Wraps MemoryFs and counts IFileSystem::listDirectory invocations (host tests only). */
class CountingMemoryFs final : public IFileSystem {
public:
    explicit CountingMemoryFs(std::string name) : _inner(std::move(name)) {}

    unsigned list_directory_calls() const { return _list_directory_calls; }

    void add_entry(const std::string& path, bool is_dir, std::uint64_t size = 0)
    {
        _inner.add_entry(path, is_dir, size);
    }

    void set_directory(const std::string& path, std::vector<FileInfo> entries)
    {
        _inner.set_directory(path, std::move(entries));
    }

    FileSystemKind kind() const override { return _inner.kind(); }
    std::string name() const override { return _inner.name(); }
    bool exists(const std::string& path) override { return _inner.exists(path); }
    bool isDirectory(const std::string& path) override { return _inner.isDirectory(path); }
    bool createDirectory(const std::string& path) override { return _inner.createDirectory(path); }
    bool removeFile(const std::string& path) override { return _inner.removeFile(path); }
    bool removeDirectory(const std::string& path) override { return _inner.removeDirectory(path); }
    bool rename(const std::string& from, const std::string& to) override
    {
        return _inner.rename(from, to);
    }
    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override
    {
        return _inner.open(path, mode);
    }
    bool stat(const std::string& path, FileInfo& outInfo) override { return _inner.stat(path, outInfo); }
    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override
    {
        ++_list_directory_calls;
        return _inner.listDirectory(path, outEntries);
    }

private:
    MemoryFs _inner;
    unsigned _list_directory_calls{0};
};

TEST_CASE("FileDevice ListDirectory accepts full URI requests")
{
    constexpr const char* kDir = "tnfs://server/ld-basic";
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory(kDir, {
        FileInfo{std::string(kDir) + "/GAMES", true, 0, {}},
        FileInfo{std::string(kDir) + "/README.TXT", false, 123, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.id = 1;
    request.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    request.payload = make_list_request(kDir, 0, kListMaxPayloadBytes);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= kListDirHeaderBytes);
    CHECK(response.payload[0] == kVersion);
    CHECK(response.payload[1] == 0);
    CHECK(read_u16le(response.payload, 4) == 0);
    CHECK(read_u16le(response.payload, 6) == 2);
}

TEST_CASE("FileDevice rejects application-state commands from its former range")
{
    StorageManager storage;
    FileDevice device(storage);
    IORequest request{};
    request.command = 0x20;
    CHECK(device.handle(request).status == StatusCode::Unsupported);
    request.command = 0x25;
    CHECK(device.handle(request).status == StatusCode::Unsupported);
}

TEST_CASE("FileDevice ListDirectory without listFlags uses full entries and clears compact in response")
{
    constexpr const char* kDir = "tnfs://server/ld-fullfmt";
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory(kDir, {
        FileInfo{std::string(kDir) + "/GAMES", true, 0, {}},
        FileInfo{std::string(kDir) + "/README.TXT", false, 123, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    request.payload = make_list_request(kDir, 0, kListMaxPayloadBytes);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= kListDirHeaderBytes);
    CHECK(response.payload[0] == kVersion);
    CHECK((response.payload[1] & 0x02U) == 0);
    CHECK(read_u16le(response.payload, 6) == 2);

    const std::uint8_t len0 = response.payload[kListDirHeaderBytes + 1];
    const std::uint8_t len1 = response.payload[kListDirHeaderBytes + list_entry_span_bytes(len0, false) + 1];
    const std::size_t entries_len =
        list_entry_span_bytes(len0, false) + list_entry_span_bytes(len1, false);
    CHECK(read_u16le(response.payload, 8) == static_cast<std::uint16_t>(entries_len));
    CHECK(response.payload.size() == kListDirHeaderBytes + entries_len);
}

TEST_CASE("FileDevice ListDirectory compact request sets compact flag and omits per-entry metadata")
{
    constexpr const char* kDir = "tnfs://server/ld-compact";
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory(kDir, {
        FileInfo{std::string(kDir) + "/GAMES", true, 0, {}},
        FileInfo{std::string(kDir) + "/README.TXT", false, 123, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    request.payload = make_list_request_with_flags(
        kDir, 0, kListMaxPayloadBytes, kListFlagCompactOmitMetadata);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= kListDirHeaderBytes);
    CHECK(response.payload[0] == kVersion);
    CHECK((response.payload[1] & 0x02U) == 0x02U);
    CHECK(read_u16le(response.payload, 6) == 2);

    const std::uint8_t len0 = response.payload[kListDirHeaderBytes + 1];
    const std::uint8_t len1 =
        response.payload[kListDirHeaderBytes + list_entry_span_bytes(len0, true) + 1];
    const std::size_t entries_len =
        list_entry_span_bytes(len0, true) + list_entry_span_bytes(len1, true);
    CHECK(read_u16le(response.payload, 8) == static_cast<std::uint16_t>(entries_len));
    CHECK(response.payload.size() == kListDirHeaderBytes + entries_len);
}

TEST_CASE("FileDevice ListDirectory sort-by-name request reorders by basename")
{
    constexpr const char* kDir = "tnfs://server/ld-sort";
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory(kDir, {
        FileInfo{std::string(kDir) + "/zebra.txt", false, 1, {}},
        FileInfo{std::string(kDir) + "/alpha.txt", false, 1, {}},
        FileInfo{std::string(kDir) + "/mid.txt", false, 1, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);

    IORequest unsorted{};
    unsorted.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    unsorted.payload = make_list_request(kDir, 0, kListMaxPayloadBytes);
    const auto r0 = device.handle(unsorted);
    CHECK(r0.status == StatusCode::Ok);
    CHECK(first_list_entry_name(r0.payload) == "zebra.txt");

    IORequest sorted{};
    sorted.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    sorted.payload =
        make_list_request_with_flags(kDir, 0, kListMaxPayloadBytes, kListFlagSortByName);
    const auto r1 = device.handle(sorted);
    CHECK(r1.status == StatusCode::Ok);
    CHECK((r1.payload[1] & 0x02U) == 0);
    CHECK(first_list_entry_name(r1.payload) == "alpha.txt");
}

TEST_CASE("FileDevice ListDirectory formatted request returns ls-style text lines")
{
    constexpr const char* kDir = "tnfs://server/ld-formatted";
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory(kDir, {
        FileInfo{std::string(kDir) + "/atari", true, 4096, {}},
        FileInfo{std::string(kDir) + "/test.txt", false, 18, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    request.payload = make_list_request_with_flags(
        kDir,
        0,
        kListMaxPayloadBytes,
        static_cast<std::uint8_t>(kListFlagSortByName | kListFlagFormattedLines));

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= kListDirHeaderBytes);
    CHECK((response.payload[1] & 0x04U) == 0x04U);
    CHECK(read_u16le(response.payload, 6) == 2);

    const std::uint16_t entries_len = read_u16le(response.payload, 8);
    const std::string_view text{
        reinterpret_cast<const char*>(response.payload.data() + kListDirHeaderBytes),
        entries_len};
    CHECK(text.find("atari") != std::string_view::npos);
    CHECK(text.find("test.txt") != std::string_view::npos);
    CHECK(text.find('\n') != std::string_view::npos);
    CHECK(text[0] == 'd');
}

TEST_CASE("FileDevice ListDirectory compact and sort flags combine")
{
    constexpr const char* kDir = "tnfs://server/ld-combo";
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory(kDir, {
        FileInfo{std::string(kDir) + "/zebra.txt", false, 1, {}},
        FileInfo{std::string(kDir) + "/alpha.txt", false, 1, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    request.payload = make_list_request_with_flags(
        kDir,
        0,
        kListMaxPayloadBytes,
        static_cast<std::uint8_t>(kListFlagCompactOmitMetadata | kListFlagSortByName));

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    CHECK((response.payload[1] & 0x02U) == 0x02U);
    CHECK(first_list_entry_name(response.payload) == "alpha.txt");
    const std::uint8_t len0 = response.payload[kListDirHeaderBytes + 1];
    CHECK(response.payload.size() == kListDirHeaderBytes + list_entry_span_bytes(len0, true) * 2);
}

TEST_CASE("FileDevice ListDirectory sets more flag when another page exists")
{
    constexpr const char* kDir = "tnfs://server/ld-more";
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory(kDir, {
        FileInfo{std::string(kDir) + "/a", false, 1, {}},
        FileInfo{std::string(kDir) + "/b", false, 1, {}},
        FileInfo{std::string(kDir) + "/c", false, 1, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    request.payload = make_list_request_with_flags(kDir, 0, 3, kListFlagCompactOmitMetadata);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= kListDirHeaderBytes);
    CHECK((response.payload[1] & 0x01U) == 0x01U);
    CHECK(read_u16le(response.payload, 6) == 1);
    CHECK(read_u16le(response.payload, 8) == 3);
}

TEST_CASE("FileDevice ListDirectory respects maxPayloadBytes for long names")
{
    constexpr const char* kDir = "tnfs://server/ld-long";
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory(kDir, {
        FileInfo{std::string(kDir) + "/short", false, 1, {}},
        FileInfo{std::string(kDir) + "/this_name_is_longer_than_twenty_six_chars.txt", false, 1, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    request.payload = make_list_request_with_flags(kDir, 0, 30, kListFlagCompactOmitMetadata);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    CHECK(read_u16le(response.payload, 6) == 1);
    CHECK(first_list_entry_name(response.payload) == "short");
    CHECK((response.payload[1] & 0x01U) == 0x01U);
    CHECK(read_u16le(response.payload, 8) <= 30);
}

TEST_CASE("FileDevice ListDirectory maxNameBytes truncates names and guarantees progress")
{
    constexpr const char* kDir = "tnfs://server/ld-name-limit";
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory(kDir, {
        FileInfo{std::string(kDir) + "/this_name_is_much_longer_than_the_client_window.ssd", false, 1, {}},
        FileInfo{std::string(kDir) + "/second.ssd", false, 1, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    request.payload = make_list_request_with_limits(
        kDir, 0, 32, kListFlagCompactOmitMetadata, 30);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= kListDirHeaderBytes + 32);
    CHECK(read_u16le(response.payload, 6) == 1);
    CHECK(read_u16le(response.payload, 8) == 32);
    CHECK((response.payload[1] & kListResponseFlagNameTruncated) == kListResponseFlagNameTruncated);
    CHECK((response.payload[kListDirHeaderBytes] & kListEntryFlagNameTruncated) == kListEntryFlagNameTruncated);
    CHECK(first_list_entry_name(response.payload) == "this_name_is_much_longer_than_");
    CHECK((response.payload[1] & 0x01U) == 0x01U);
}

TEST_CASE("FileDevice ListDirectory caches listing and skips repeated filesystem reads")
{
    constexpr const char* kDirA = "tnfs://server/ld-cache-a";
    constexpr const char* kDirB = "tnfs://server/ld-cache-b";

    StorageManager storage;
    auto fs = std::make_unique<CountingMemoryFs>("tnfs");
    CountingMemoryFs* const spy = fs.get();
    spy->set_directory(kDirA, {
        FileInfo{std::string(kDirA) + "/one.txt", false, 1, {}},
    });
    spy->set_directory(kDirB, {
        FileInfo{std::string(kDirB) + "/two.txt", false, 1, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest req{};
    req.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);

    req.payload = make_list_request(kDirA, 0, kListMaxPayloadBytes);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 1);

    req.payload = make_list_request(kDirA, 0, kListMaxPayloadBytes);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 1);

    req.payload = make_list_request(kDirB, 0, kListMaxPayloadBytes);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 2);

    req.payload = make_list_request(kDirB, 0, kListMaxPayloadBytes);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 2);

    req.payload = make_list_request(kDirA, 0, kListMaxPayloadBytes);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 3);
}

TEST_CASE("HostService manipulates current host and LRU history")
{
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->add_entry("tnfs://server/a", true);
    fs->add_entry("tnfs://server/b", true);
    fs->add_entry("tnfs://server/c", true);
    CHECK(storage.registerFileSystem(std::move(fs)));
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));

    HostService device(storage);

    IORequest request{};
    request.command = static_cast<std::uint16_t>(HostCommand::SetCurrent);
    request.payload = make_host_set_request("tnfs://server/a");
    CHECK(device.handle(request).status == StatusCode::Ok);
    request.payload = make_host_set_request("tnfs://server/b");
    CHECK(device.handle(request).status == StatusCode::Ok);
    request.payload = make_host_set_request("tnfs://server/c");
    CHECK(device.handle(request).status == StatusCode::Ok);

    CHECK(host_history_text(device) ==
          "0 tnfs://server/c\n1 tnfs://server/b\n2 tnfs://server/a\n");

    request.command = static_cast<std::uint16_t>(HostCommand::SelectHistory);
    request.payload = make_host_index_request(2);
    CHECK(device.handle(request).status == StatusCode::Ok);
    CHECK(host_current_uri(device) == "tnfs://server/a");
    CHECK(host_history_text(device) ==
          "0 tnfs://server/a\n1 tnfs://server/c\n2 tnfs://server/b\n");

    request.command = static_cast<std::uint16_t>(HostCommand::DeleteHistory);
    request.payload = make_host_index_request(0);
    CHECK(device.handle(request).status == StatusCode::Ok);
    CHECK(host_current_uri(device) == "tnfs://server/a");
    CHECK(host_history_text(device) == "0 tnfs://server/c\n1 tnfs://server/b\n");
}

TEST_CASE("AppStore current-host key is plain key/value storage")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));

    AppStore store(storage);
    AppStore::WriteResult wr{};
    const std::string raw = "not/a/canonical/host";
    CHECK(store.write("fujinet-nio", "current-host", 0,
                      reinterpret_cast<const std::uint8_t*>(raw.data()),
                      static_cast<std::uint16_t>(raw.size()), wr));
    CHECK(wr.written == raw.size());

    AppStore::ReadResult rr{};
    CHECK(store.read("fujinet-nio", "current-host", 0, 128, rr));
    REQUIRE(rr.exists);
    CHECK(std::string(rr.data.begin(), rr.data.end()) == raw);
}

TEST_CASE("FileDevice ListDirectory resolves empty and relative specs through current host")
{
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->add_entry("tnfs://server/root", true);
    fs->add_entry("tnfs://server/root/NEXT", true);
    fs->set_directory("tnfs://server/root", {
        FileInfo{"tnfs://server/root/NEXT", true, 0, {}},
    });
    fs->set_directory("tnfs://server/root/NEXT", {
        FileInfo{"tnfs://server/root/NEXT/file.txt", false, 1, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));

    FileDevice device(storage);

    AppStore store(storage);
    AppStore::WriteResult write{};
    const std::string currentHost = "tnfs://server/root";
    CHECK(store.write(
        "fujinet-nio", "current-host", 0,
        reinterpret_cast<const std::uint8_t*>(currentHost.data()),
        static_cast<std::uint16_t>(currentHost.size()), write));

    IORequest list{};
    list.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    list.payload = make_list_request("", 0, kListMaxPayloadBytes);
    const auto current_response = device.handle(list);
    CHECK(current_response.status == StatusCode::Ok);
    REQUIRE(current_response.payload.size() >= kListDirHeaderBytes + 2);
    CHECK(first_list_entry_name(current_response.payload) == "NEXT");

    list.payload = make_list_request("NEXT", 0, kListMaxPayloadBytes);
    const auto relative_response = device.handle(list);
    CHECK(relative_response.status == StatusCode::Ok);
    REQUIRE(relative_response.payload.size() >= kListDirHeaderBytes + 2);
    CHECK(first_list_entry_name(relative_response.payload) == "file.txt");
}

TEST_CASE("FileDevice ListDirectory rejects relative specs without current host")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));
    FileDevice device(storage);

    IORequest list{};
    list.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    list.payload = make_list_request("", 0, kListMaxPayloadBytes);
    const auto response = device.handle(list);
    CHECK(response.status == StatusCode::DeviceNotFound);
}

TEST_CASE("FileDevice resolves persist URI through default persistent filesystem")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("flash")));
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("host")));

    FileDevice device(storage);

    IORequest mkdir{};
    mkdir.command = static_cast<std::uint16_t>(FileCommand::MakeDirectory);
    mkdir.payload = make_mkdir_request("persist:///FujiNet", true, true);
    CHECK(device.handle(mkdir).status == StatusCode::Ok);

    IORequest write{};
    write.command = static_cast<std::uint16_t>(FileCommand::WriteFile);
    write.payload = make_write_request("persist:///FujiNet/fe0c0101.key", 0, "legacy-key");
    const auto write_response = device.handle(write);
    CHECK(write_response.status == StatusCode::Ok);
    REQUIRE(write_response.payload.size() >= 10);
    CHECK(read_u16le(write_response.payload, 8) == 10);

    IORequest read{};
    read.command = static_cast<std::uint16_t>(FileCommand::ReadFile);
    read.payload = make_read_request("persist:///FujiNet/fe0c0101.key", 0, 64);
    const auto read_response = device.handle(read);
    CHECK(read_response.status == StatusCode::Ok);
    REQUIRE(read_response.payload.size() >= 10);
    CHECK(read_u16le(read_response.payload, 8) == 10);
    const std::string value(read_response.payload.begin() + 10, read_response.payload.end());
    CHECK(value == "legacy-key");

    auto* host = storage.get("host");
    REQUIRE(host != nullptr);
    CHECK(host->exists("/FujiNet/fe0c0101.key"));

    auto* flash = storage.get("flash");
    REQUIRE(flash != nullptr);
    CHECK_FALSE(flash->exists("/FujiNet/fe0c0101.key"));
}

TEST_CASE("FileDevice ResolvePath returns canonical URI and display path for tnfs target")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("tnfs")));

    FileDevice device(storage);

    auto* tnfs = storage.get("tnfs");
    REQUIRE(tnfs != nullptr);
    CHECK(tnfs->createDirectory("/bbc"));
    {
        auto f = tnfs->open("/bbc/bwc.ssd", "wb");
        REQUIRE(f != nullptr);
        CHECK(f->write("x", 1) == 1);
    }

    IORequest req{};
    req.command = static_cast<std::uint16_t>(FileCommand::ResolvePath);
    req.payload = make_resolve_path_request("tnfs://192.168.1.101/bbc/", "bwc.ssd");
    const auto response = device.handle(req);

    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= 8);
    CHECK(response.payload[0] == kVersion);
    CHECK((response.payload[1] & 0x01U) == 0x00U);
    CHECK((response.payload[1] & 0x02U) == 0x00U);

    std::size_t off = 4;
    const auto resolved = read_len_string(response.payload, off);
    const auto display = read_len_string(response.payload, off);
    CHECK(resolved == "tnfs://192.168.1.101/bbc/bwc.ssd");
    CHECK(display == "/bbc/bwc.ssd");
}

TEST_CASE("FileDevice ResolvePath canonicalizes base URI when arg is empty")
{
    StorageManager storage;
    CHECK(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("tnfs")));

    FileDevice device(storage);

    auto* tnfs = storage.get("tnfs");
    REQUIRE(tnfs != nullptr);
    CHECK(tnfs->createDirectory("/bbc"));

    IORequest req{};
    req.command = static_cast<std::uint16_t>(FileCommand::ResolvePath);
    req.payload = make_resolve_path_request("tnfs://192.168.1.101/bbc/", "");
    const auto response = device.handle(req);

    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= 8);
    std::size_t off = 4;
    const auto resolved = read_len_string(response.payload, off);
    const auto display = read_len_string(response.payload, off);
    CHECK(resolved == "tnfs://192.168.1.101/bbc");
    CHECK(display == "/bbc");
}

} // namespace
