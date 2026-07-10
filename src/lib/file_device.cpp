#include "fujinet/io/devices/file_device.h"

#include "fujinet/core/logging.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/io/devices/app_store.h"
#include "fujinet/io/host_state.h"
#include "fujinet/io/core/io_message.h"

// Commands + to_file_command helper
#include "fujinet/io/devices/file_commands.h"

// Binary codec
#include "fujinet/io/devices/file_codec.h"
#include "fujinet/io/list_directory_format.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::io {

using fujinet::fs::FileInfo;
using fujinet::fs::IFile;
using fujinet::fs::IFileSystem;
using fujinet::fs::StorageManager;
using fujinet::io::fileproto::Reader;

// Uncomment if we do any logging in here
// static constexpr const char* TAG = "io";

static constexpr std::uint8_t FILEPROTO_VERSION = 1;
static constexpr auto LIST_DIRECTORY_CACHE_TTL = std::chrono::seconds(120);

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

static bool parse_common_prefix(Reader& r, CommonPrefix& out, bool allow_empty = false)
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

    if (!allow_empty && out.uri.empty()) return false;

    return true;
}

struct AppStorePrefix {
    std::string ns;
    std::string key;
};

static bool parse_app_store_prefix(Reader& r, AppStorePrefix& out, bool require_key)
{
    std::uint8_t ver = 0;
    if (!r.read_u8(ver) || ver != FILEPROTO_VERSION) {
        return false;
    }

    std::uint16_t ns_len = 0;
    if (!r.read_u16le(ns_len) || ns_len == 0) return false;
    const std::uint8_t* ns_ptr = nullptr;
    if (!r.read_bytes(ns_ptr, ns_len)) return false;
    out.ns.assign(reinterpret_cast<const char*>(ns_ptr), ns_len);

    std::uint16_t key_len = 0;
    if (!r.read_u16le(key_len)) return false;
    const std::uint8_t* key_ptr = nullptr;
    if (key_len > 0) {
        if (!r.read_bytes(key_ptr, key_len)) return false;
        out.key.assign(reinterpret_cast<const char*>(key_ptr), key_len);
    } else {
        out.key.clear();
    }

    if (!AppStore::valid_namespace(out.ns)) return false;
    if (require_key && !AppStore::valid_key(out.key)) return false;
    if (!require_key && !out.key.empty()) return false;
    return true;
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
        case protocol::FileCommand::MakeDirectory:
            return handle_make_directory(request);
        case protocol::FileCommand::AppStoreStat:
            return handle_app_store_stat(request);
        case protocol::FileCommand::AppStoreRead:
            return handle_app_store_read(request);
        case protocol::FileCommand::AppStoreWrite:
            return handle_app_store_write(request);
        case protocol::FileCommand::AppStoreDelete:
            return handle_app_store_delete(request);
        case protocol::FileCommand::AppStoreList:
            return handle_app_store_list(request);
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
// After uri + (startIndex, maxPayloadBytes), clients may send an optional u8 listFlags (if
// payload has a byte left). listDirectory() on a filesystem has already collected the full
// directory before this handler paginates, so sort-by-basename (flag) reorders a copy, not a
// stream. maxPayloadBytes caps the variable entries blob; only whole entries are encoded.
IOResponse FileDevice::handle_list_directory(const IORequest& request)
{
    using fujinet::io::protocol::list_directory::kListFlagCompactOmitMetadata;
    using fujinet::io::protocol::list_directory::kListFlagFormattedLines;
    using fujinet::io::protocol::list_directory::kListFlagSortByName;
    using fujinet::io::protocol::list_directory::kListResponseFlagFormatted;

    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p, true)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint16_t startIndex = 0;
    std::uint16_t maxPayloadBytes = 0;
    if (!r.read_u16le(startIndex) || !r.read_u16le(maxPayloadBytes) || maxPayloadBytes == 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint8_t listFlags = 0;
    if (r.remaining() > 0) {
        if (!r.read_u8(listFlags)) {
            resp.status = StatusCode::InvalidRequest;
            return resp;
        }
    }

    const bool formatted = (listFlags & kListFlagFormattedLines) != 0;
    const bool compact = (listFlags & kListFlagCompactOmitMetadata) != 0;
    const bool sortName = (listFlags & kListFlagSortByName) != 0;

    if (formatted && compact) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    HostState hostState(_storage);
    std::string canonical_uri;
    if (!hostState.resolve_target(p.uri, canonical_uri, nullptr)) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    auto [fs, resolvedPath] = _storage.resolveUri(canonical_uri);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    std::vector<FileInfo> entries;
    if (!get_cached_directory_entries(canonical_uri, entries)) {
        if (!fs->listDirectory(resolvedPath, entries)) {
            resp.status = StatusCode::IOError;
            return resp;
        }
        store_cached_directory_entries(canonical_uri, entries);
    }

    auto basename_sv = [](const std::string& s) -> std::string_view {
        const auto pos = s.find_last_of('/');
        if (pos == std::string::npos) {
            return std::string_view{s};
        }
        if (pos + 1 >= s.size()) {
            return std::string_view{s};
        }
        return std::string_view{s}.substr(pos + 1);
    };

    const std::vector<FileInfo>* source = &entries;
    std::vector<FileInfo> sorted;
    if (sortName) {
        sorted = entries;
        std::sort(
            sorted.begin(), sorted.end(), [&](const FileInfo& a, const FileInfo& b) { return basename_sv(a.path) < basename_sv(b.path); });
        source = &sorted;
    }

    const std::size_t total = source->size();
    const std::size_t start = (startIndex < total) ? startIndex : total;

    // Response:
    // u8 version
    // u8 flags: bit0=more, bit1=compact (entries have no u64 size or u64 mtime)
    // u16 reserved
    // u16 startIndex (echo)
    // u16 entryCount
    // u16 entriesLen
    // entries...
    std::string out;
    out.reserve(64 + maxPayloadBytes);

    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, 0); // flags placeholder
    fileproto::write_u16le(out, 0);
    fileproto::write_u16le(out, startIndex);

    const std::size_t entryCountPos = out.size();
    fileproto::write_u16le(out, 0); // entryCount placeholder
    const std::size_t entriesLenPos = out.size();
    fileproto::write_u16le(out, 0); // entriesLen placeholder

    const std::size_t entriesStart = out.size();
    std::uint16_t returned = 0;

    if (formatted) {
        for (std::size_t i = start; i < total; ++i) {
            const auto& e = (*source)[i];
            const auto name = basename_sv(e.path);
            const std::string line = format_list_directory_line(e, name);
            const std::size_t entriesUsed = out.size() - entriesStart;
            if (entriesUsed + line.size() > maxPayloadBytes) {
                break;
            }
            out.append(line);
            ++returned;
        }
    } else {
        for (std::size_t i = start; i < total; ++i) {
            const auto& e = (*source)[i];
            const auto name = basename_sv(e.path);

            const std::uint8_t nameLen =
                static_cast<std::uint8_t>(std::min<std::size_t>(name.size(), 220));
            const std::size_t entryBytes =
                2U + static_cast<std::size_t>(nameLen) + (compact ? 0U : 16U);
            const std::size_t entriesUsed = out.size() - entriesStart;
            if (entriesUsed + entryBytes > maxPayloadBytes) {
                break;
            }

            std::uint8_t eflags = e.isDirectory ? 0x01 : 0x00;
            fileproto::write_u8(out, eflags);
            fileproto::write_u8(out, nameLen);
            fileproto::write_bytes(out, name.data(), nameLen);

            if (!compact) {
                fileproto::write_u64le(out, e.sizeBytes);
                fileproto::write_u64le(out, to_unix_seconds(e.modifiedTime));
            }
            ++returned;
        }
    }

    const std::size_t entriesLen = out.size() - entriesStart;
    const bool more = (start + returned < total);

    std::uint8_t flags = 0;
    if (more) {
        flags |= 0x01U;
    }
    if (compact) {
        flags |= 0x02U;
    }
    if (formatted) {
        flags |= kListResponseFlagFormatted;
    }
    out[1] = static_cast<char>(flags);

    out[entryCountPos + 0] = static_cast<char>(returned & 0xFF);
    out[entryCountPos + 1] = static_cast<char>((returned >> 8) & 0xFF);
    out[entriesLenPos + 0] = static_cast<char>(entriesLen & 0xFF);
    out[entriesLenPos + 1] = static_cast<char>((entriesLen >> 8) & 0xFF);

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

IOResponse FileDevice::handle_app_store_stat(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    AppStorePrefix p{};
    if (!parse_app_store_prefix(r, p, true)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    AppStore store(_storage);
    if (!store.available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::Stat st{};
    if (!store.stat(p.ns, p.key, st)) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    std::string out;
    out.reserve(1 + 1 + 2 + 8 + 8);
    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, st.exists ? 0x01U : 0x00U);
    fileproto::write_u16le(out, 0);
    fileproto::write_u64le(out, st.sizeBytes);
    fileproto::write_u64le(out, st.modifiedUnixTime);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse FileDevice::handle_app_store_read(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    AppStorePrefix p{};
    if (!parse_app_store_prefix(r, p, true)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint32_t offset = 0;
    std::uint16_t max_bytes = 0;
    if (!r.read_u32le(offset) || !r.read_u16le(max_bytes) || max_bytes == 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    AppStore store(_storage);
    if (!store.available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::ReadResult result{};
    HostState hostState(_storage);
    if (p.ns == HostState::kNamespace && p.key == HostState::kHostHistoryListKey) {
        std::string text;
        if (!hostState.format_history(&text)) {
            resp.status = StatusCode::IOError;
            return resp;
        }
        result.exists = true;
        result.offset = offset;
        if (offset < text.size()) {
            const std::size_t n = std::min<std::size_t>(max_bytes, text.size() - offset);
            result.data.assign(text.begin() + static_cast<std::ptrdiff_t>(offset),
                               text.begin() + static_cast<std::ptrdiff_t>(offset + n));
        }
        result.eof = static_cast<std::uint64_t>(offset) + result.data.size() >= text.size();
    } else if (!store.read(p.ns, p.key, offset, max_bytes, result)) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    std::uint8_t flags = 0;
    if (result.eof) flags |= 0x01U;
    if (result.exists) flags |= 0x02U;

    std::string out;
    out.reserve(1 + 1 + 2 + 4 + 2 + result.data.size());
    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, flags);
    fileproto::write_u16le(out, 0);
    fileproto::write_u32le(out, result.offset);
    fileproto::write_u16le(out, static_cast<std::uint16_t>(result.data.size()));
    if (!result.data.empty()) {
        fileproto::write_bytes(out, result.data.data(), result.data.size());
    }
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse FileDevice::handle_app_store_write(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    AppStorePrefix p{};
    if (!parse_app_store_prefix(r, p, true)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint32_t offset = 0;
    std::uint16_t data_len = 0;
    if (!r.read_u32le(offset) || !r.read_u16le(data_len)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    const std::uint8_t* data = nullptr;
    if (!r.read_bytes(data, data_len)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    AppStore store(_storage);
    if (!store.available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::WriteResult result{};
    HostState hostState(_storage);
    if (p.ns == HostState::kNamespace && p.key == HostState::kCurrentHostKey) {
        result.offset = offset;
        const std::string spec(reinterpret_cast<const char*>(data), data_len);
        if (offset != 0 || !hostState.set_current_host(spec)) {
            resp.status = StatusCode::IOError;
            return resp;
        }
        result.written = data_len;
    } else if (p.ns == HostState::kNamespace && p.key == HostState::kCurrentHostIndexKey) {
        result.offset = offset;
        const std::string indexText(reinterpret_cast<const char*>(data), data_len);
        if (offset != 0 || !hostState.set_current_host_index(indexText)) {
            resp.status = StatusCode::IOError;
            return resp;
        }
        result.written = data_len;
    } else if (p.ns == HostState::kNamespace && p.key == HostState::kHostHistoryDeleteKey) {
        result.offset = offset;
        const std::string indexText(reinterpret_cast<const char*>(data), data_len);
        if (offset != 0 || !hostState.delete_history_index(indexText)) {
            resp.status = StatusCode::IOError;
            return resp;
        }
        result.written = data_len;
    } else if (!store.write(p.ns, p.key, offset, data, data_len, result)) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    std::string out;
    out.reserve(1 + 1 + 2 + 4 + 2);
    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, 0);
    fileproto::write_u16le(out, 0);
    fileproto::write_u32le(out, result.offset);
    fileproto::write_u16le(out, result.written);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse FileDevice::handle_app_store_delete(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    AppStorePrefix p{};
    if (!parse_app_store_prefix(r, p, true)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    AppStore store(_storage);
    if (!store.available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::DeleteResult result{};
    if (!store.remove(p.ns, p.key, result)) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    std::string out;
    out.reserve(1 + 1 + 2);
    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, result.deleted ? 0x01U : 0x00U);
    fileproto::write_u16le(out, 0);
    resp.payload.assign(out.begin(), out.end());
    return resp;
}

IOResponse FileDevice::handle_app_store_list(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    AppStorePrefix p{};
    if (!parse_app_store_prefix(r, p, false)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint16_t start_index = 0;
    std::uint16_t max_payload_bytes = 0;
    if (!r.read_u16le(start_index) || !r.read_u16le(max_payload_bytes) || max_payload_bytes == 0) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    AppStore store(_storage);
    if (!store.available()) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    AppStore::ListResult result{};
    if (!store.list(p.ns, start_index, max_payload_bytes, result)) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    std::string out;
    out.reserve(1 + 1 + 2 + 2 + 2 + 2 + max_payload_bytes);
    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, result.more ? 0x01U : 0x00U);
    fileproto::write_u16le(out, 0);
    fileproto::write_u16le(out, result.startIndex);
    fileproto::write_u16le(out, static_cast<std::uint16_t>(result.keys.size()));

    const std::size_t keys_len_pos = out.size();
    fileproto::write_u16le(out, 0);
    const std::size_t keys_start = out.size();
    for (const auto& key : result.keys) {
        fileproto::write_u16le(out, static_cast<std::uint16_t>(key.size()));
        fileproto::write_bytes(out, key.data(), key.size());
    }
    const auto keys_len = static_cast<std::uint16_t>(out.size() - keys_start);
    out[keys_len_pos + 0] = static_cast<char>(keys_len & 0xFF);
    out[keys_len_pos + 1] = static_cast<char>((keys_len >> 8) & 0xFF);

    resp.payload.assign(out.begin(), out.end());
    return resp;
}

} // namespace fujinet::io
