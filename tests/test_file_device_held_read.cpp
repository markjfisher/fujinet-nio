#include "doctest.h"

// FileDevice keeps the file from the last ReadFile open for the next one on a
// network filesystem. These run through the real TNFS filesystem over a fake
// server, whose open-handle count shows when FileDevice lets the file go.
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/http_filesystem.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/file_commands.h"
#include "fujinet/io/devices/file_device.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fake_fs.h"
#include "fake_tnfs_server.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
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
using fujinet::io::IOResponse;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FileCommand;
using fujinet::tests::FakeTnfsServer;

constexpr std::size_t kBlock = 512;

void append_u16le(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void append_u32le(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::vector<std::uint8_t> uri_prefix(std::string_view uri)
{
    std::vector<std::uint8_t> p{1};
    append_u16le(p, static_cast<std::uint16_t>(uri.size()));
    p.insert(p.end(), uri.begin(), uri.end());
    return p;
}

struct Read {
    StatusCode status{StatusCode::IOError};
    bool eof{false};
    std::vector<std::uint8_t> data;
};

Read read_file(FileDevice& device, std::string_view uri, std::uint32_t offset,
               std::uint16_t maxBytes = kBlock)
{
    IORequest req{};
    req.command = static_cast<std::uint16_t>(FileCommand::ReadFile);
    req.payload = uri_prefix(uri);
    append_u32le(req.payload, offset);
    append_u16le(req.payload, maxBytes);
    const IOResponse resp = device.handle(req);

    Read out;
    out.status = resp.status;
    if (resp.status == StatusCode::Ok) {
        REQUIRE(resp.payload.size() >= 10);
        out.eof = (resp.payload[1] & 0x01) != 0;
        const std::size_t len = resp.payload[8] | (resp.payload[9] << 8);
        REQUIRE(resp.payload.size() == 10 + len);
        out.data.assign(resp.payload.begin() + 10, resp.payload.end());
    }
    return out;
}

StatusCode write_file(FileDevice& device, std::string_view uri, std::uint32_t offset,
                      const std::vector<std::uint8_t>& data)
{
    IORequest req{};
    req.command = static_cast<std::uint16_t>(FileCommand::WriteFile);
    req.payload = uri_prefix(uri);
    append_u32le(req.payload, offset);
    append_u16le(req.payload, static_cast<std::uint16_t>(data.size()));
    req.payload.insert(req.payload.end(), data.begin(), data.end());
    return device.handle(req).status;
}

std::vector<std::uint8_t> pattern(std::size_t bytes, std::uint8_t seed)
{
    std::vector<std::uint8_t> v(bytes);
    for (std::size_t i = 0; i < bytes; ++i) v[i] = static_cast<std::uint8_t>(i * 7 + i / kBlock + seed);
    return v;
}

std::vector<std::uint8_t> slice(const std::vector<std::uint8_t>& v, std::size_t at, std::size_t n)
{
    return {v.begin() + static_cast<std::ptrdiff_t>(at),
            v.begin() + static_cast<std::ptrdiff_t>(at + n)};
}

struct TnfsRig {
    std::shared_ptr<FakeTnfsServer> server = std::make_shared<FakeTnfsServer>(true);
    StorageManager storage;

    TnfsRig() { REQUIRE(storage.registerFileSystem(fujinet::fs::make_tnfs_filesystem(server))); }
};

// A local filesystem that counts opens and the files still open.
class CountingLocalFs final : public IFileSystem {
public:
    fujinet::tests::MemoryFileSystem inner{"host"};
    int opens = 0;
    int openNow = 0;

    FileSystemKind kind() const override { return FileSystemKind::HostPosix; }
    std::string name() const override { return "host"; }
    bool exists(const std::string& p) override { return inner.exists(p); }
    bool isDirectory(const std::string& p) override { return inner.isDirectory(p); }
    bool createDirectory(const std::string& p) override { return inner.createDirectory(p); }
    bool removeFile(const std::string& p) override { return inner.removeFile(p); }
    bool removeDirectory(const std::string& p) override { return inner.removeDirectory(p); }
    bool rename(const std::string& a, const std::string& b) override { return inner.rename(a, b); }
    bool stat(const std::string& p, FileInfo& out) override { return inner.stat(p, out); }
    bool listDirectory(const std::string& p, std::vector<FileInfo>& out) override
    {
        return inner.listDirectory(p, out);
    }
    std::unique_ptr<IFile> open(const std::string& p, const char* mode) override
    {
        auto f = inner.open(p, mode);
        if (!f) return nullptr;
        ++opens;
        return std::make_unique<Counted>(std::move(f), openNow);
    }

private:
    class Counted final : public IFile {
    public:
        Counted(std::unique_ptr<IFile> f, int& live) : _f(std::move(f)), _live(live) { ++_live; }
        ~Counted() override { --_live; }
        std::size_t read(void* d, std::size_t n) override { return _f->read(d, n); }
        std::size_t write(const void* s, std::size_t n) override { return _f->write(s, n); }
        bool seek(std::uint64_t o) override { return _f->seek(o); }
        std::uint64_t tell() const override { return _f->tell(); }
        bool flush() override { return _f->flush(); }

    private:
        std::unique_ptr<IFile> _f;
        int& _live;
    };
};

// A one-file HTTP protocol that counts the fetches made of it.
class CountingHttp final : public fujinet::io::INetworkProtocol {
public:
    CountingHttp(std::vector<std::uint8_t> body, int& fetches) : _body(std::move(body)), _fetches(fetches) {}

    StatusCode open(const fujinet::io::NetworkOpenRequest&) override
    {
        ++_fetches;
        return StatusCode::Ok;
    }
    StatusCode write_body(std::uint32_t, const std::uint8_t*, std::size_t, std::uint16_t& written) override
    {
        written = 0;
        return StatusCode::Unsupported;
    }
    StatusCode read_body(std::uint32_t offset, std::uint8_t* out, std::size_t outLen, std::uint16_t& read,
                         bool& eof, bool& more) override
    {
        read = 0;
        const std::size_t n = offset >= _body.size() ? 0 : std::min(outLen, _body.size() - offset);
        if (n > 0) {
            std::memcpy(out, _body.data() + offset, n);
            read = static_cast<std::uint16_t>(n);
        }
        eof = offset + n >= _body.size();
        more = !eof;
        return StatusCode::Ok;
    }
    StatusCode info(fujinet::io::NetworkInfo& out) override
    {
        out = fujinet::io::NetworkInfo{};
        out.hasHttpStatus = true;
        out.httpStatus = 200;
        out.hasContentLength = true;
        out.contentLength = _body.size();
        return StatusCode::Ok;
    }
    void poll() override {}
    void close() override {}

private:
    std::vector<std::uint8_t> _body;
    int& _fetches;
};

} // namespace

TEST_CASE("FileDevice: a TNFS file read in many ReadFile requests is opened once")
{
    TnfsRig rig;
    const auto file = pattern(20 * kBlock - 100, 1);
    rig.server->files["/app.prc"] = file;
    FileDevice device(rig.storage);

    std::vector<std::uint8_t> got;
    int requests = 0;
    for (;;) {
        const Read r = read_file(device, "tnfs://server/app.prc", static_cast<std::uint32_t>(got.size()));
        REQUIRE(r.status == StatusCode::Ok);
        ++requests;
        got.insert(got.end(), r.data.begin(), r.data.end());
        if (r.eof) break;
    }

    CHECK(requests == 20);
    CHECK(got == file);
    // The file and its seven read-ahead handles, opened for the first request only (it was
    // eight opens a request), and every 8 blocks fetched together.
    CHECK(rig.server->opens == 8);
    CHECK(rig.server->batches == 3);
    // The short last read ended the file, so its handles went back.
    CHECK(rig.server->open_handles() == 0);
}

TEST_CASE("FileDevice: a held TNFS file serves reads at any offset")
{
    TnfsRig rig;
    const auto file = pattern(16 * kBlock, 2);
    rig.server->files["/app.prc"] = file;
    FileDevice device(rig.storage);

    CHECK(read_file(device, "tnfs://server/app.prc", 3 * kBlock).data == slice(file, 3 * kBlock, kBlock));
    CHECK(read_file(device, "tnfs://server/app.prc", 100).data == slice(file, 100, kBlock));
    CHECK(read_file(device, "tnfs://server/app.prc", 12 * kBlock + 7).data ==
          slice(file, 12 * kBlock + 7, kBlock));
    CHECK(rig.server->opens == 8);
}

TEST_CASE("FileDevice: reading another file gives the held one back first")
{
    TnfsRig rig;
    rig.server->files["/a.prc"] = pattern(16 * kBlock, 3);
    rig.server->files["/b.prc"] = pattern(16 * kBlock, 4);
    FileDevice device(rig.storage);

    REQUIRE(read_file(device, "tnfs://server/a.prc", 0).status == StatusCode::Ok);
    CHECK(rig.server->open_handles() == 8);
    CHECK(read_file(device, "tnfs://server/b.prc", 0).data == slice(rig.server->files["/b.prc"], 0, kBlock));
    CHECK(rig.server->open_handles() == 8); // b's only
    CHECK(rig.server->opens == 16);
}

TEST_CASE("FileDevice: WriteFile gives a held read back, so the next read sees the write")
{
    TnfsRig rig;
    rig.server->files["/app.prc"] = pattern(16 * kBlock, 5);
    FileDevice device(rig.storage);

    // Caches blocks 0-7 in the held file.
    REQUIRE(read_file(device, "tnfs://server/app.prc", 0).status == StatusCode::Ok);
    REQUIRE(rig.server->open_handles() == 8);
    const std::vector<std::uint8_t> ones(kBlock, 0x11);
    REQUIRE(write_file(device, "tnfs://server/app.prc", kBlock, ones) == StatusCode::Ok);
    CHECK(rig.server->open_handles() == 0);

    CHECK(read_file(device, "tnfs://server/app.prc", kBlock).data == ones);
}

TEST_CASE("FileDevice: a held file is given back when it has been idle")
{
    TnfsRig rig;
    rig.server->files["/app.prc"] = pattern(16 * kBlock, 6);

    SUBCASE("poll keeps a file read just now")
    {
        FileDevice device(rig.storage);
        REQUIRE(read_file(device, "tnfs://server/app.prc", 0).status == StatusCode::Ok);
        device.poll();
        CHECK(rig.server->open_handles() == 8);
    }
    SUBCASE("poll gives back a file idle for the limit")
    {
        FileDevice device(rig.storage, std::chrono::milliseconds{0});
        REQUIRE(read_file(device, "tnfs://server/app.prc", 0).status == StatusCode::Ok);
        CHECK(rig.server->open_handles() == 8);
        device.poll();
        CHECK(rig.server->open_handles() == 0);
    }
    SUBCASE("a read after the limit opens the file again, without poll")
    {
        FileDevice device(rig.storage, std::chrono::milliseconds{0});
        REQUIRE(read_file(device, "tnfs://server/app.prc", 0).status == StatusCode::Ok);
        REQUIRE(read_file(device, "tnfs://server/app.prc", kBlock).status == StatusCode::Ok);
        CHECK(rig.server->opens == 16);
        CHECK(rig.server->open_handles() == 8);
    }
}

TEST_CASE("FileDevice: a held handle the server has forgotten is opened again")
{
    TnfsRig rig;
    const auto file = pattern(16 * kBlock, 7);
    rig.server->files["/app.prc"] = file;
    FileDevice device(rig.storage);

    REQUIRE(read_file(device, "tnfs://server/app.prc", 0).status == StatusCode::Ok);
    REQUIRE(rig.server->open_handles() == 8);
    rig.server->forget_handles(); // the server restarted

    // Block 8 is not cached, so the held handles are asked and come back empty.
    const Read r = read_file(device, "tnfs://server/app.prc", 8 * kBlock);
    CHECK(r.status == StatusCode::Ok);
    CHECK_FALSE(r.eof);
    CHECK(r.data == slice(file, 8 * kBlock, kBlock));
    CHECK(rig.server->opens == 16);
}

TEST_CASE("FileDevice: a local file is closed after every ReadFile")
{
    StorageManager storage;
    auto fs = std::make_unique<CountingLocalFs>();
    CountingLocalFs* const local = fs.get();
    REQUIRE(local->inner.create_file("/app.prc", pattern(4 * kBlock, 8)));
    REQUIRE(storage.registerFileSystem(std::move(fs)));
    FileDevice device(storage);

    REQUIRE(read_file(device, "persist:///app.prc", 0).status == StatusCode::Ok);
    CHECK(local->openNow == 0);
    REQUIRE(read_file(device, "persist:///app.prc", kBlock).status == StatusCode::Ok);
    CHECK(local->opens == 2);
    CHECK(local->openNow == 0);
}

TEST_CASE("FileDevice: a held handle gone stale does not cut a read short at the cache edge")
{
    TnfsRig rig;
    const auto file = pattern(32 * kBlock, 9);
    rig.server->files["/app.prc"] = file;
    FileDevice device(rig.storage);

    REQUIRE(read_file(device, "tnfs://server/app.prc", 0).status == StatusCode::Ok); // blocks 0-7 cached
    rig.server->forget_handles();

    // The first 100 bytes are still cached; the rest are not, and the held handles are gone.
    // Reporting those 100 bytes as the end would end the host's read 412 bytes early.
    const Read r = read_file(device, "tnfs://server/app.prc", 8 * kBlock - 100);
    CHECK(r.status == StatusCode::Ok);
    CHECK_FALSE(r.eof);
    CHECK(r.data == slice(file, 8 * kBlock - 100, kBlock));
}

TEST_CASE("FileDevice: a file of an exact multiple of the read size ends without opening it again")
{
    TnfsRig rig;
    const auto file = pattern(8 * kBlock, 10);
    rig.server->files["/app.prc"] = file;
    FileDevice device(rig.storage);

    std::vector<std::uint8_t> got;
    for (std::uint32_t at = 0; at < file.size(); at += kBlock) {
        const Read r = read_file(device, "tnfs://server/app.prc", at);
        REQUIRE(r.status == StatusCode::Ok);
        got.insert(got.end(), r.data.begin(), r.data.end());
    }
    CHECK(got == file);

    // The host cannot tell the file has ended until a read comes back empty.
    const Read last = read_file(device, "tnfs://server/app.prc", static_cast<std::uint32_t>(file.size()));
    CHECK(last.status == StatusCode::Ok);
    CHECK(last.eof);
    CHECK(last.data.empty());
    // That read is the end of the file, not a stale handle: no second set of handles.
    CHECK(rig.server->opens == 8);
    CHECK(rig.server->open_handles() == 0);
}

TEST_CASE("FileDevice: another open on the session takes the held file's read-ahead handles, and reads go on")
{
    TnfsRig rig;
    const auto a = pattern(32 * kBlock, 11);
    const auto b = pattern(32 * kBlock, 12);
    rig.server->files["/a.prc"] = a;
    rig.server->files["/b.prc"] = b;
    rig.server->maxOpen = 8; // the held file's eight handles are the session's whole allowance
    FileDevice device(rig.storage);

    REQUIRE(read_file(device, "tnfs://server/a.prc", 0).data == slice(a, 0, kBlock));
    REQUIRE(rig.server->open_handles() == 8);

    // Something else on the session (a disk image mount, say) opens a file through the same filesystem.
    auto [fs, path] = rig.storage.resolveUri("tnfs://server/b.prc");
    REQUIRE(fs != nullptr);
    auto other = fs->open(path, "rb");
    REQUIRE(other != nullptr);
    std::vector<std::uint8_t> head(kBlock);
    REQUIRE(other->read(head.data(), head.size()) == kBlock);
    CHECK(head == slice(b, 0, kBlock));

    // The held file lost its read-ahead handles to that open but still reads correctly.
    for (std::uint32_t at = kBlock; at < 12 * kBlock; at += kBlock) {
        CHECK(read_file(device, "tnfs://server/a.prc", at).data == slice(a, at, kBlock));
    }
}

TEST_CASE("FileDevice: an HTTP file read in many ReadFile requests is fetched once")
{
    int fetches = 0;
    const auto file = pattern(5 * kBlock - 30, 13);
    StorageManager storage;
    REQUIRE(storage.registerFileSystem(fujinet::fs::make_http_filesystem(
        [&](std::string_view) -> std::unique_ptr<fujinet::io::INetworkProtocol> {
            return std::make_unique<CountingHttp>(file, fetches);
        })));
    FileDevice device(storage);

    std::vector<std::uint8_t> got;
    for (;;) {
        const Read r = read_file(device, "http://example.com/app.prc", static_cast<std::uint32_t>(got.size()));
        REQUIRE(r.status == StatusCode::Ok);
        got.insert(got.end(), r.data.begin(), r.data.end());
        if (r.eof) break;
    }
    CHECK(got == file);
    CHECK(fetches == 1);
}
