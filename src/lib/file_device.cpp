#include "fujinet/io/devices/file_device.h"

#include "fujinet/core/logging.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/io/core/io_message.h"

// Commands + to_file_command helper
#include "fujinet/io/devices/file_commands.h"

// Binary codec
#include "fujinet/io/devices/file_codec.h"
#include "fujinet/fs/path_resolvers/path_resolver.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string_view>
#include <vector>

namespace fujinet::io {

using fujinet::fs::FileInfo;
using fujinet::fs::IFile;
using fujinet::fs::IFileSystem;
using fujinet::fs::StorageManager;
using fujinet::io::fileproto::Reader;

// Uncomment if we do any logging in here
// static const char* TAG = "io";

static constexpr std::uint8_t FILEPROTO_VERSION = 1;
static constexpr auto LIST_DIRECTORY_CACHE_TTL = std::chrono::seconds(20);

struct ListDirectoryCache {
    std::string uri;
    std::vector<FileInfo> entries;
    std::chrono::steady_clock::time_point expires_at{};
    bool valid{false};
};

static ListDirectoryCache g_list_directory_cache{};

static bool get_cached_directory_entries(const std::string& uri, std::vector<FileInfo>& out)
{
    if (!g_list_directory_cache.valid) {
        return false;
    }

    if (g_list_directory_cache.uri != uri) {
        return false;
    }

    if (std::chrono::steady_clock::now() > g_list_directory_cache.expires_at) {
        g_list_directory_cache.valid = false;
        return false;
    }

    out = g_list_directory_cache.entries;
    g_list_directory_cache.expires_at = std::chrono::steady_clock::now() + LIST_DIRECTORY_CACHE_TTL;
    return true;
}

static void store_cached_directory_entries(const std::string& uri, const std::vector<FileInfo>& entries)
{
    g_list_directory_cache.uri = uri;
    g_list_directory_cache.entries = entries;
    g_list_directory_cache.expires_at = std::chrono::steady_clock::now() + LIST_DIRECTORY_CACHE_TTL;
    g_list_directory_cache.valid = true;
}

// Common request prefix:
// u8 version
// u16 uriLen (LE)
// u8[] uri   - full URI (e.g., "tnfs://host:port/path" or "sd0:/path")
struct CommonPrefix {
    std::string uri;
};

static bool parse_common_prefix(Reader& r, CommonPrefix& out)
{
    std::uint8_t ver = 0;
    if (!r.read_u8(ver) || ver != FILEPROTO_VERSION) {
        return false;
    }

    std::uint16_t uriLen = 0;
    if (!r.read_u16le(uriLen)) return false;

    const std::uint8_t* uriPtr = nullptr;
    if (!r.read_bytes(uriPtr, uriLen)) return false;
    out.uri.assign(reinterpret_cast<const char*>(uriPtr), uriLen);

    if (out.uri.empty()) return false;

    return true;
}

struct ResolvePathRequest {
    std::string base_uri;
    std::string arg;
};

static bool parse_resolve_path_request(Reader& r, ResolvePathRequest& out)
{
    std::uint8_t ver = 0;
    if (!r.read_u8(ver) || ver != FILEPROTO_VERSION) {
        return false;
    }

    std::uint16_t base_uri_len = 0;
    if (!r.read_u16le(base_uri_len) || base_uri_len == 0) return false;
    const std::uint8_t* base_uri_ptr = nullptr;
    if (!r.read_bytes(base_uri_ptr, base_uri_len)) return false;
    out.base_uri.assign(reinterpret_cast<const char*>(base_uri_ptr), base_uri_len);

    std::uint16_t arg_len = 0;
    if (!r.read_u16le(arg_len)) return false;
    const std::uint8_t* arg_ptr = nullptr;
    if (arg_len > 0) {
        if (!r.read_bytes(arg_ptr, arg_len)) return false;
        out.arg.assign(reinterpret_cast<const char*>(arg_ptr), arg_len);
    } else {
        out.arg.clear();
    }

    return true;
}

static bool make_path_context(const std::string& base_uri, fs::PathContext& ctx)
{
    fs::PathResolver resolver;
    fs::ResolvedTarget target;
    if (!resolver.resolve(base_uri, {}, target)) {
        return false;
    }
    ctx.cwd_fs = target.fs_name;
    ctx.cwd_path = target.fs_path;
    return true;
}

static std::string build_resolved_uri(const fs::ResolvedTarget& target)
{
    if (target.fs_path.find("://") != std::string::npos) {
        return target.fs_path;
    }
    return target.fs_name + ":" + target.fs_path;
}

static std::string build_display_path(const fs::ResolvedTarget& target)
{
    std::string path = target.fs_path;

    const auto scheme_pos = path.find("://");
    if (scheme_pos != std::string::npos) {
        const auto slash_pos = path.find('/', scheme_pos + 3);
        if (slash_pos != std::string::npos) {
            path = path.substr(slash_pos);
        } else {
            path = "/";
        }
    } else {
        const auto prefix_pos = path.find(':');
        if (prefix_pos != std::string::npos) {
            path = path.substr(prefix_pos + 1);
        }
    }

    if (path.empty()) {
        path = "/";
    }
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return path;
}

static std::uint64_t to_unix_seconds(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{}) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count());
}

FileDevice::FileDevice(StorageManager& storage)
    : _storage(storage)
{}

IOResponse FileDevice::handle(const IORequest& request)
{
    auto cmd = protocol::to_file_command(request.command);

    switch (cmd) {
        case protocol::FileCommand::Stat:
            return handle_stat(request);
        case protocol::FileCommand::ListDirectory:
            return handle_list_directory(request);
        case protocol::FileCommand::ReadFile:
            return handle_read_file(request);
        case protocol::FileCommand::WriteFile:
            return handle_write_file(request);
        case protocol::FileCommand::ResolvePath:
            return handle_resolve_path(request);
        case protocol::FileCommand::MakeDirectory:
            return handle_make_directory(request);
        default:
            return make_base_response(request, StatusCode::Unsupported);
    }
}

static std::string normalize_dir_path(std::string p)
{
    // Trim trailing slashes except root.
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    // Force absolute within FS (convention).
    if (!p.empty() && p.front() != '/') p.insert(p.begin(), '/');
    return p;
}

static bool mkdir_parents(IFileSystem& fs, const std::string& path)
{
    // Create each prefix directory in order: /a, /a/b, /a/b/c
    std::string p = normalize_dir_path(path);
    if (p.empty() || p == "/") return true;

    std::string cur;
    cur.reserve(p.size());

    std::size_t i = 0;
    while (i < p.size()) {
        // Skip repeated '/'
        while (i < p.size() && p[i] == '/') ++i;
        if (i >= p.size()) break;

        std::size_t j = i;
        while (j < p.size() && p[j] != '/') ++j;

        const std::string part = p.substr(i, j - i);
        cur += "/";
        cur += part;

        if (!fs.createDirectory(cur) && !fs.isDirectory(cur)) {
            return false;
        }
        i = j;
    }
    return true;
}

// --------------------
// Stat (0x01)
// --------------------
IOResponse FileDevice::handle_stat(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto [fs, resolvedPath] = _storage.resolveUri(p.uri);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    FileInfo info{};
    const bool exists = fs->stat(resolvedPath, info);

    std::uint8_t flags = 0;
    if (exists) {
        flags |= 0x02; // exists
        if (info.isDirectory) flags |= 0x01; // isDir
    }

    const std::uint64_t size  = exists ? info.sizeBytes : 0;
    const std::uint64_t mtime = exists ? to_unix_seconds(info.modifiedTime) : 0;

    // Build response payload:
    // u8 version
    // u8 flags (bit0=isDir, bit1=exists)
    // u16 reserved=0
    // u64 sizeBytes
    // u64 modifiedUnixTime
    std::string out;
    out.reserve(1 + 1 + 2 + 8 + 8);

    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, flags);
    fileproto::write_u16le(out, 0);
    fileproto::write_u64le(out, size);
    fileproto::write_u64le(out, mtime);

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

// --------------------
// ListDirectory (0x02)
// --------------------
IOResponse FileDevice::handle_list_directory(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint16_t startIndex = 0;
    std::uint16_t maxEntries = 0;
    if (!r.read_u16le(startIndex) || !r.read_u16le(maxEntries) || maxEntries == 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto [fs, resolvedPath] = _storage.resolveUri(p.uri);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    std::vector<FileInfo> entries;
    if (!get_cached_directory_entries(p.uri, entries)) {
        if (!fs->listDirectory(resolvedPath, entries)) {
            resp.status = StatusCode::IOError;
            return resp;
        }
        store_cached_directory_entries(p.uri, entries);
    }

    // Basename helper
    auto basename = [](const std::string& s) -> std::string_view {
        auto pos = s.find_last_of('/');
        if (pos == std::string::npos) return std::string_view{s};
        if (pos + 1 >= s.size()) return std::string_view{s};
        return std::string_view{s}.substr(pos + 1);
    };

    const std::size_t total = entries.size();
    const std::size_t start = (startIndex < total) ? startIndex : total;
    const std::size_t end = std::min<std::size_t>(start + maxEntries, total);
    const std::uint16_t returned = static_cast<std::uint16_t>(end - start);
    const bool more = (end < total);

    // Response:
    // u8 version
    // u8 flags bit0=more
    // u16 reserved
    // u16 returnedCount
    // entries...
    std::string out;
    out.reserve(64 + returned * 32);

    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, more ? 0x01 : 0x00);
    fileproto::write_u16le(out, 0);
    fileproto::write_u16le(out, returned);

    for (std::size_t i = start; i < end; ++i) {
        const auto& e = entries[i];
        const auto name = basename(e.path);

        std::uint8_t eflags = e.isDirectory ? 0x01 : 0x00;
        // ... if there are more flags, then they can grow here
        fileproto::write_u8(out, eflags);

        const std::uint8_t nameLen =
            static_cast<std::uint8_t>(std::min<std::size_t>(name.size(), 255));
        fileproto::write_u8(out, nameLen);
        fileproto::write_bytes(out, name.data(), nameLen);

        fileproto::write_u64le(out, e.sizeBytes);
        fileproto::write_u64le(out, to_unix_seconds(e.modifiedTime));
    }

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

// --------------------
// ReadFile (0x03)
// --------------------
IOResponse FileDevice::handle_read_file(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint32_t offset = 0;
    std::uint16_t maxBytes = 0;
    if (!r.read_u32le(offset) || !r.read_u16le(maxBytes) || maxBytes == 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto [fs, resolvedPath] = _storage.resolveUri(p.uri);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    auto file = fs->open(resolvedPath, "rb");
    if (!file) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    if (!file->seek(static_cast<std::uint64_t>(offset))) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    // Response:
    // u8 version
    // u8 flags bit0=eof bit1=truncated
    // u16 reserved
    // u32 offset (echo)
    // u16 dataLen
    // data...
    std::string out;
    out.reserve(1 + 1 + 2 + 4 + 2 + maxBytes);

    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, 0); // flags placeholder
    fileproto::write_u16le(out, 0);
    fileproto::write_u32le(out, offset);

    const std::size_t dataLenPos = out.size();
    fileproto::write_u16le(out, 0); // placeholder

    const std::size_t dataStart = out.size();
    out.resize(dataStart + maxBytes);

    const std::size_t n = file->read(out.data() + dataStart, maxBytes);

    out.resize(dataStart + n);

    // Fill dataLen
    out[dataLenPos + 0] = static_cast<char>(n & 0xFF);
    out[dataLenPos + 1] = static_cast<char>((n >> 8) & 0xFF);

    // Best-effort flags:
    // eof if we couldn't fill the request (n < maxBytes)
    // truncated if we filled exactly maxBytes (caller may need another read)
    std::uint8_t flags = 0;
    if (n < maxBytes) flags |= 0x01;       // eof-ish
    if (n == maxBytes) flags |= 0x02;      // truncated-ish (more may exist)
    out[1] = static_cast<char>(flags);

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

// --------------------
// WriteFile (0x04)
// --------------------
IOResponse FileDevice::handle_write_file(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint32_t offset = 0;
    std::uint16_t dataLen = 0;
    if (!r.read_u32le(offset) || !r.read_u16le(dataLen)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    const std::uint8_t* dataPtr = nullptr;
    if (!r.read_bytes(dataPtr, dataLen)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto [fs, resolvedPath] = _storage.resolveUri(p.uri);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    // v1 open mode convention:
    // offset==0 => create/truncate
    // offset>0  => open existing read/write (best effort)
    const char* mode = (offset == 0) ? "wb" : "r+b";
    auto file = fs->open(resolvedPath, mode);
    if (!file && offset > 0) {
        // If file didn't exist, allow creation when offset>0 too.
        file = fs->open(resolvedPath, "wb");
    }
    if (!file) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    if (offset > 0 && !file->seek(static_cast<std::uint64_t>(offset))) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    const std::size_t written = file->write(dataPtr, dataLen);
    (void)file->flush();

    // Response:
    // u8 version
    // u8 flags (0)
    // u16 reserved
    // u32 offset
    // u16 writtenLen
    std::string out;
    out.reserve(1 + 1 + 2 + 4 + 2);

    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, 0); // flags
    fileproto::write_u16le(out, 0);
    fileproto::write_u32le(out, offset);
    fileproto::write_u16le(out, static_cast<std::uint16_t>(written));

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse FileDevice::handle_resolve_path(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    ResolvePathRequest p{};
    if (!parse_resolve_path_request(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    fs::ResolvedTarget resolved;
    if (p.arg.empty()) {
        fs::PathResolver resolver;
        if (!resolver.resolve(p.base_uri, {}, resolved)) {
            resp.status = StatusCode::DeviceNotFound;
            return resp;
        }
    } else {
        fs::PathContext ctx;
        if (!make_path_context(p.base_uri, ctx)) {
            resp.status = StatusCode::DeviceNotFound;
            return resp;
        }

        fs::PathResolver resolver;
        if (!resolver.resolve(p.arg, ctx, resolved)) {
            resp.status = StatusCode::InvalidRequest;
            return resp;
        }
    }

    auto [resolved_fs, resolved_path] = _storage.resolveUri(build_resolved_uri(resolved));
    if (!resolved_fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    FileInfo info{};
    const bool exists = resolved_fs->stat(resolved_path, info);

    std::uint8_t flags = 0;
    if (exists) {
        flags |= 0x02;
        if (info.isDirectory) flags |= 0x01;
    }

    const std::string resolved_uri = build_resolved_uri(resolved);
    const std::string display_path = build_display_path(resolved);

    std::string out;
    out.reserve(1 + 1 + 2 + 2 + resolved_uri.size() + 2 + display_path.size());
    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, flags);
    fileproto::write_u16le(out, 0);
    fileproto::write_u16le(out, static_cast<std::uint16_t>(resolved_uri.size()));
    fileproto::write_bytes(out, resolved_uri.data(), resolved_uri.size());
    fileproto::write_u16le(out, static_cast<std::uint16_t>(display_path.size()));
    fileproto::write_bytes(out, display_path.data(), display_path.size());

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

// --------------------
// MakeDirectory (0x05)
// --------------------
IOResponse FileDevice::handle_make_directory(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint8_t flags = 0;
    if (!r.read_u8(flags)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    const bool parents = (flags & 0x01) != 0;
    const bool existOk = (flags & 0x02) != 0;

    auto [fs, resolvedPath] = _storage.resolveUri(p.uri);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    const std::string dirPath = normalize_dir_path(resolvedPath);

    bool ok = false;
    if (parents) {
        ok = mkdir_parents(*fs, dirPath);
    } else {
        ok = fs->createDirectory(dirPath);
    }

    if (!ok) {
        if (existOk && fs->isDirectory(dirPath)) {
            ok = true;
        }
    }

    if (!ok) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    // Response payload:
    // u8 version
    // u8 flags (0)
    // u16 reserved
    std::string out;
    out.reserve(1 + 1 + 2);
    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, 0);
    fileproto::write_u16le(out, 0);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

} // namespace fujinet::io
