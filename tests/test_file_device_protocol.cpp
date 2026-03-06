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

TEST_CASE("FileDevice ListDirectory accepts full URI requests")
{
    StorageManager storage;
    auto fs = std::make_unique<MemoryFs>("tnfs");
    fs->set_directory("tnfs://server/share", {
        FileInfo{"tnfs://server/share/GAMES", true, 0, {}},
        FileInfo{"tnfs://server/share/README.TXT", false, 123, {}},
    });
    CHECK(storage.registerFileSystem(std::move(fs)));

    FileDevice device(storage);
    IORequest request{};
    request.id = 1;
    request.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    request.payload = make_list_request("tnfs://server/share", 0, 8);

    const auto response = device.handle(request);
    CHECK(response.status == StatusCode::Ok);
    REQUIRE(response.payload.size() >= 5);
    CHECK(response.payload[0] == kVersion);
    CHECK(response.payload[1] == 0);
    CHECK(read_u16le(response.payload, 4) == 2);
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

} // namespace
