#include "doctest.h"

#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/file_commands.h"
#include "fujinet/io/devices/file_device.h"

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
using fujinet::io::FileDevice;
using fujinet::io::IORequest;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FileCommand;
using fujinet::io::protocol::list_directory::kListFlagCompactOmitMetadata;
using fujinet::io::protocol::list_directory::kListFlagSortByName;

constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kListDirHeaderBytes = 6;

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

std::vector<std::uint8_t> make_list_request(std::string_view uri, std::uint16_t start, std::uint16_t max)
{
    auto payload = make_uri_request(uri);
    append_u16le(payload, start);
    append_u16le(payload, max);
    return payload;
}

std::vector<std::uint8_t> make_list_request_with_flags(
    std::string_view uri, std::uint16_t start, std::uint16_t max, std::uint8_t list_flags)
{
    auto payload = make_list_request(uri, start, max);
    append_u8(payload, list_flags);
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

std::vector<std::uint8_t> make_resolve_request(std::string_view base_uri, std::string_view arg)
{
    std::vector<std::uint8_t> payload;
    append_u8(payload, kVersion);
    append_u16le(payload, static_cast<std::uint16_t>(base_uri.size()));
    payload.insert(payload.end(), base_uri.begin(), base_uri.end());
    append_u16le(payload, static_cast<std::uint16_t>(arg.size()));
    payload.insert(payload.end(), arg.begin(), arg.end());
    return payload;
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
    request.payload = make_list_request(kDir, 0, 8);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= 5);
    CHECK(response.payload[0] == kVersion);
    CHECK(response.payload[1] == 0);
    CHECK(read_u16le(response.payload, 4) == 2);
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
    request.payload = make_list_request(kDir, 0, 8);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= kListDirHeaderBytes);
    CHECK(response.payload[0] == kVersion);
    CHECK((response.payload[1] & 0x02U) == 0);
    CHECK(read_u16le(response.payload, 4) == 2);

    const std::uint8_t len0 = response.payload[kListDirHeaderBytes + 1];
    const std::uint8_t len1 = response.payload[kListDirHeaderBytes + list_entry_span_bytes(len0, false) + 1];
    const std::size_t expected =
        kListDirHeaderBytes + list_entry_span_bytes(len0, false) + list_entry_span_bytes(len1, false);
    CHECK(response.payload.size() == expected);
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
    request.payload =
        make_list_request_with_flags(kDir, 0, 8, kListFlagCompactOmitMetadata);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= kListDirHeaderBytes);
    CHECK(response.payload[0] == kVersion);
    CHECK((response.payload[1] & 0x02U) == 0x02U);
    CHECK(read_u16le(response.payload, 4) == 2);

    const std::uint8_t len0 = response.payload[kListDirHeaderBytes + 1];
    const std::uint8_t len1 =
        response.payload[kListDirHeaderBytes + list_entry_span_bytes(len0, true) + 1];
    const std::size_t expected =
        kListDirHeaderBytes + list_entry_span_bytes(len0, true) + list_entry_span_bytes(len1, true);
    CHECK(response.payload.size() == expected);
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
    unsorted.payload = make_list_request(kDir, 0, 8);
    const auto r0 = device.handle(unsorted);
    CHECK(r0.status == StatusCode::Ok);
    CHECK(first_list_entry_name(r0.payload) == "zebra.txt");

    IORequest sorted{};
    sorted.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    sorted.payload = make_list_request_with_flags(kDir, 0, 8, kListFlagSortByName);
    const auto r1 = device.handle(sorted);
    CHECK(r1.status == StatusCode::Ok);
    CHECK((r1.payload[1] & 0x02U) == 0);
    CHECK(first_list_entry_name(r1.payload) == "alpha.txt");
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
        kDir, 0, 8, static_cast<std::uint8_t>(kListFlagCompactOmitMetadata | kListFlagSortByName));

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
    request.payload = make_list_request(kDir, 0, 1);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= kListDirHeaderBytes);
    CHECK((response.payload[1] & 0x01U) == 0x01U);
    CHECK(read_u16le(response.payload, 4) == 1);
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

    req.payload = make_list_request(kDirA, 0, 8);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 1);

    req.payload = make_list_request(kDirA, 0, 8);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 1);

    req.payload = make_list_request(kDirB, 0, 8);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 2);

    req.payload = make_list_request(kDirB, 0, 8);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 2);

    req.payload = make_list_request(kDirA, 0, 8);
    CHECK(device.handle(req).status == StatusCode::Ok);
    CHECK(spy->list_directory_calls() == 3);
}

TEST_CASE("FileDevice ResolvePath resolves relative TNFS paths")
{
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->add_entry("tnfs://server/root/NEXT", true);
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.id = 2;
    request.command = static_cast<std::uint16_t>(FileCommand::ResolvePath);
    request.payload = make_resolve_request("tnfs://server/root", "NEXT");

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= 8);
    CHECK(response.payload[0] == kVersion);
    CHECK((response.payload[1] & 0x03) == 0x03);

    std::size_t offset = 4;
    const std::string resolved_uri = read_len_string(response.payload, offset);
    const std::string display_path = read_len_string(response.payload, offset);

    CHECK(resolved_uri == "tnfs://server/root/NEXT");
    CHECK(display_path == "/root/NEXT");
}

TEST_CASE("FileDevice ResolvePath rejects unresolved base URIs")
{
    StorageManager storage;
    FileDevice device(storage);

    IORequest request{};
    request.id = 3;
    request.command = static_cast<std::uint16_t>(FileCommand::ResolvePath);
    request.payload = make_resolve_request("tnfs://server/root", "NEXT");

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::DeviceNotFound);
}

TEST_CASE("FileDevice ResolvePath canonicalizes full URI when arg is empty")
{
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->add_entry("tnfs://server/root/NEXT", true);
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.id = 4;
    request.command = static_cast<std::uint16_t>(FileCommand::ResolvePath);
    request.payload = make_resolve_request("tnfs://server/root/NEXT", "");

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= 8);

    std::size_t offset = 4;
    const std::string resolved_uri = read_len_string(response.payload, offset);
    const std::string display_path = read_len_string(response.payload, offset);

    CHECK(resolved_uri == "tnfs://server/root/NEXT");
    CHECK(display_path == "/root/NEXT");
}

TEST_CASE("FileDevice ResolvePath returns IOError when resolved target probe fails")
{
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.id = 5;
    request.command = static_cast<std::uint16_t>(FileCommand::ResolvePath);
    request.payload = make_resolve_request("tnfs://server/root", "NEXT");

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::IOError);
    CHECK(response.payload.empty());
}

} // namespace
