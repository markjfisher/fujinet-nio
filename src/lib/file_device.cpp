#include "fujinet/io/devices/file_device.h"

#include "fujinet/core/logging.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/path_resolvers/path_resolver.h"
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
#include <limits>
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

static std::uint64_t to_unix_seconds(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{}) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count());
}

/* ASCII case-insensitive glob. A bare pattern is a substring, while * and ?
 * have their usual glob meanings. Kept here because ListPages owns its view. */
static bool name_matches_filter(std::string_view name, const std::string& pattern)
{
    auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
    std::string glob = pattern;
    if (glob.find_first_of("*?") == std::string::npos) glob = "*" + glob + "*";
    std::size_t ni = 0, pi = 0, star = std::string::npos, retry = 0;
    while (ni < name.size()) {
        if (pi < glob.size() && (glob[pi] == '?' || fold(glob[pi]) == fold(name[ni]))) { ++pi; ++ni; }
        else if (pi < glob.size() && glob[pi] == '*') { star = pi++; retry = ni; }
        else if (star != std::string::npos) { pi = star + 1; ni = ++retry; }
        else return false;
    }
    while (pi < glob.size() && glob[pi] == '*') ++pi;
    return pi == glob.size();
}

FileDevice::FileDevice(StorageManager& storage, std::chrono::milliseconds readIdle)
    : _storage(storage)
    , _readIdle(readIdle)
{}

void FileDevice::poll()
{
    if (_heldRead.file && std::chrono::steady_clock::now() - _heldRead.lastUse >= _readIdle) {
        _heldRead = HeldRead{};
    }
}

// The file held from the last ReadFile when it is the same one and has not been idle too
// long; otherwise a new open, after the held file (and its TNFS handles) is given back.
std::unique_ptr<IFile> FileDevice::open_for_read(IFileSystem& fs, const std::string& path,
                                                 bool& reused)
{
    HeldRead held = std::move(_heldRead);
    _heldRead = HeldRead{};
    reused = held.file && held.fs == &fs && held.path == path &&
             std::chrono::steady_clock::now() - held.lastUse < _readIdle;
    if (reused) {
        return std::move(held.file);
    }
    held.file.reset();
    return fs.open(path, "rb");
}

IOResponse FileDevice::handle(const IORequest& request)
{
    auto cmd = protocol::to_file_command(request.command);

    switch (cmd) {
        case protocol::FileCommand::Stat:
            return handle_stat(request);
        case protocol::FileCommand::ListDirectory:
            return handle_list_directory(request);
        case protocol::FileCommand::ListPages:
            return handle_list_pages(request);
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

static std::string build_resolve_uri(const fujinet::fs::ResolvedTarget& target)
{
    if (target.fs_path.find("://") != std::string::npos) {
        return target.fs_path;
    }
    return target.fs_name + ":" + target.fs_path;
}

static std::string build_resolve_display_path(const fujinet::fs::ResolvedTarget& target)
{
    std::string path = target.display_path.empty() ? target.fs_path : target.display_path;

    const auto scheme_pos = path.find("://");
    if (scheme_pos != std::string::npos) {
        const auto slash_pos = path.find('/', scheme_pos + 3);
        path = (slash_pos == std::string::npos) ? "/" : path.substr(slash_pos);
    } else {
        const auto prefix_pos = path.find(':');
        if (prefix_pos != std::string::npos) {
            path = path.substr(prefix_pos + 1);
        }
    }

    if (path.empty()) path = "/";
    if (path.front() != '/') path.insert(path.begin(), '/');
    return path;
}

IOResponse FileDevice::handle_resolve_path(const IORequest& request)
{
    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    std::uint8_t ver = 0;
    if (!r.read_u8(ver) || ver != FILEPROTO_VERSION) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint16_t base_len = 0;
    const std::uint8_t* base_ptr = nullptr;
    std::uint16_t arg_len = 0;
    const std::uint8_t* arg_ptr = nullptr;
    std::string base_uri;
    std::string arg;
    std::string resolved_uri;
    std::string display_path;
    std::uint8_t flags = 0;

    if (!r.read_u16le(base_len) || base_len == 0 ||
        !r.read_bytes(base_ptr, base_len) ||
        !r.read_u16le(arg_len) ||
        !r.read_bytes(arg_ptr, arg_len)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    base_uri.assign(reinterpret_cast<const char*>(base_ptr), base_len);
    arg.assign(reinterpret_cast<const char*>(arg_ptr), arg_len);

    fujinet::fs::PathResolver resolver;
    fujinet::fs::ResolvedTarget target;
    if (arg.empty()) {
        if (!resolver.resolve(base_uri, {}, target)) {
            resp.status = StatusCode::DeviceNotFound;
            return resp;
        }
    } else {
        fujinet::fs::ResolvedTarget base_target;
        fujinet::fs::PathContext ctx;
        if (!resolver.resolve(base_uri, {}, base_target)) {
            resp.status = StatusCode::DeviceNotFound;
            return resp;
        }
        ctx.cwd_fs = base_target.fs_name;
        ctx.cwd_path = base_target.fs_path;
        if (!resolver.resolve(arg, ctx, target)) {
            resp.status = StatusCode::DeviceNotFound;
            return resp;
        }
    }

    resolved_uri = build_resolve_uri(target);
    display_path = build_resolve_display_path(target);

    auto [fs, resolved_path] = _storage.resolveUri(resolved_uri);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    if (fs->exists(resolved_path)) flags |= 0x02U;
    if (fs->isDirectory(resolved_path)) flags |= 0x01U;

    fileproto::write_u8(resp.payload, FILEPROTO_VERSION);
    fileproto::write_u8(resp.payload, flags);
    fileproto::write_u16le(resp.payload, 0);
    fileproto::write_u16le(resp.payload, static_cast<std::uint16_t>(resolved_uri.size()));
    resp.payload.insert(resp.payload.end(), resolved_uri.begin(), resolved_uri.end());
    fileproto::write_u16le(resp.payload, static_cast<std::uint16_t>(display_path.size()));
    resp.payload.insert(resp.payload.end(), display_path.begin(), display_path.end());
    return resp;
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
    using fujinet::io::protocol::list_directory::kListLegacyMaxNameBytes;
    using fujinet::io::protocol::list_directory::kListEntryFlagDirectory;
    using fujinet::io::protocol::list_directory::kListEntryFlagNameTruncated;
    using fujinet::io::protocol::list_directory::kListResponseFlagFormatted;
    using fujinet::io::protocol::list_directory::kListResponseFlagNameTruncated;

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

    std::uint8_t maxNameBytes = kListLegacyMaxNameBytes;
    if (r.remaining() > 0) {
        if (!r.read_u8(maxNameBytes)) {
            resp.status = StatusCode::InvalidRequest;
            return resp;
        }
        if (maxNameBytes == 0) {
            maxNameBytes = kListLegacyMaxNameBytes;
        }
    }

    const bool formatted = (listFlags & kListFlagFormattedLines) != 0;
    const bool compact = (listFlags & kListFlagCompactOmitMetadata) != 0;
    const bool sortName = (listFlags & kListFlagSortByName) != 0;

    if (formatted && compact) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (!formatted && maxPayloadBytes < 2U + (compact ? 0U : 16U)) {
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

    // Sorted/paged listing comes from the shared directory cache; only the entries this
    // reply can use are fetched, in windows.
    const fs::DirSort sortOrder = sortName ? fs::DirSort::Name : fs::DirSort::None;
    constexpr std::size_t kLegacyWindow = 32;
    std::vector<FileInfo> window;
    std::size_t total = 0;
    std::size_t windowStart = startIndex;
    {
        const auto st = _dirCache.get_range(*fs, canonical_uri, resolvedPath, sortOrder, false, windowStart,
                                            kLegacyWindow, window, total);
        if (st != fs::ListStatus::Ok) {
            resp.status = (st == fs::ListStatus::Timeout) ? StatusCode::Timeout : StatusCode::IOError;
            return resp;
        }
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

    const std::size_t start = (startIndex < total) ? startIndex : total;

    // Next entry at index i, fetching another window when needed.
    auto entry_at = [&](std::size_t i) -> const FileInfo* {
        if (i >= total) return nullptr;
        if (i < windowStart || i - windowStart >= window.size()) {
            windowStart = i;
            std::size_t t = 0;
            if (_dirCache.get_range(*fs, canonical_uri, resolvedPath, sortOrder, false, windowStart,
                                    kLegacyWindow, window, t) != fs::ListStatus::Ok) {
                window.clear();
                return nullptr;
            }
        }
        return (i - windowStart < window.size()) ? &window[i - windowStart] : nullptr;
    };

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
    bool truncatedName = false;

    if (formatted) {
        for (std::size_t i = start; i < total; ++i) {
            const FileInfo* ep = entry_at(i);
            if (ep == nullptr) break;
            const auto& e = *ep;
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
            const FileInfo* ep = entry_at(i);
            if (ep == nullptr) break;
            const auto& e = *ep;
            const auto name = basename_sv(e.path);

            const std::size_t entryOverhead = 2U + (compact ? 0U : 16U);
            const std::size_t payloadNameLimit =
                (maxPayloadBytes > entryOverhead) ? (maxPayloadBytes - entryOverhead) : 0U;
            const std::uint8_t nameLen = static_cast<std::uint8_t>(
                std::min<std::size_t>({name.size(), maxNameBytes, payloadNameLimit, kListLegacyMaxNameBytes}));
            const bool entryNameTruncated = nameLen < name.size();
            const std::size_t entryBytes =
                2U + static_cast<std::size_t>(nameLen) + (compact ? 0U : 16U);
            const std::size_t entriesUsed = out.size() - entriesStart;
            if (entriesUsed + entryBytes > maxPayloadBytes) {
                break;
            }

            std::uint8_t eflags = e.isDirectory ? kListEntryFlagDirectory : 0x00;
            if (entryNameTruncated) {
                eflags |= kListEntryFlagNameTruncated;
                truncatedName = true;
            }
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
    if (truncatedName) {
        flags |= kListResponseFlagNameTruncated;
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
// ListPages (0x07)
// --------------------
// Whole pages only. Request after the common prefix: u16 startPage, u8 pageSize (1..64),
// u16 maxPayloadBytes, u8 listFlags, u8 maxNameBytes, optional u8 filterLen + filter.
IOResponse FileDevice::handle_list_pages(const IORequest& request)
{
    namespace lp = fujinet::io::protocol::list_pages;

    auto resp = make_success_response(request);

    Reader r(request.payload.data(), request.payload.size());
    CommonPrefix p{};
    if (!parse_common_prefix(r, p, true)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    std::uint16_t startPage = 0;
    std::uint8_t pageSize = 0;
    std::uint16_t maxPayloadBytes = 0;
    std::uint8_t listFlags = 0;
    std::uint8_t maxNameBytes = 0;
    if (!r.read_u16le(startPage) || !r.read_u8(pageSize) || !r.read_u16le(maxPayloadBytes) ||
        !r.read_u8(listFlags) || !r.read_u8(maxNameBytes)) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    std::string filter;
    if (r.remaining()) {
        std::uint8_t filterLen = 0;
        const std::uint8_t* filterBytes = nullptr;
        if (!r.read_u8(filterLen) || !r.read_bytes(filterBytes, filterLen)) {
            resp.status = StatusCode::InvalidRequest;
            return resp;
        }
        filter.assign(reinterpret_cast<const char*>(filterBytes), filterLen);
    }
    if (pageSize == 0 || pageSize > lp::kMaxPageSize) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }
    if (maxNameBytes == 0 || maxNameBytes > lp::kDefaultMaxNameBytes) {
        maxNameBytes = lp::kDefaultMaxNameBytes;
    }
    // Smallest possible page: u8 count + u16 dataLen + one entry (flags, nameLen, size, mtime).
    constexpr std::size_t kPageHeader = 3U;
    constexpr std::size_t kEntryFixed = 2U + 8U;
    if (maxPayloadBytes < kPageHeader + kEntryFixed) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    const bool sortName = (listFlags & lp::kFlagSortByName) != 0;
    const bool dirsFirst = sortName && (listFlags & lp::kFlagDirsFirst) != 0;
    const bool refresh = (listFlags & lp::kFlagRefresh) != 0;
    const bool totalOnly = (listFlags & lp::kFlagTotalOnly) != 0;
    const fs::DirSort sortOrder =
        dirsFirst ? fs::DirSort::NameDirsFirst : (sortName ? fs::DirSort::Name : fs::DirSort::None);

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

    // One page worth of entries is fetched at a time (the cache fetches chunks underneath);
    // further pages only if the byte budget still has room.
    const std::size_t firstIndex = static_cast<std::size_t>(startPage) * pageSize;
    std::vector<FileInfo> window;
    std::size_t total = 0;
    std::size_t windowStart = firstIndex;
    const std::size_t want = pageSize;
    {
        // total-only still needs one fetch to learn the total.
        // Include the filter in the cache key. Filtering after a full listing keeps totals
        // and page boundaries correct, including TNFS servers without 1.1 range listing.
        const std::string cacheKey = filter.empty() ? canonical_uri : canonical_uri + "\nfilter:" + filter;
        const auto st = _dirCache.get_range(*fs, cacheKey, resolvedPath, sortOrder, refresh,
                                            filter.empty() ? windowStart : 0,
                                            filter.empty() ? (totalOnly ? 1 : want) : std::numeric_limits<std::size_t>::max(),
                                            window, total);
        if (st != fs::ListStatus::Ok) {
            resp.status = (st == fs::ListStatus::Timeout) ? StatusCode::Timeout : StatusCode::IOError;
            return resp;
        }
        if (!filter.empty()) {
            std::vector<FileInfo> filtered;
            for (const auto& e : window) {
                const auto slash = e.path.find_last_of('/');
                const std::string_view name = slash == std::string::npos ? std::string_view(e.path)
                                                                            : std::string_view(e.path).substr(slash + 1);
                if (e.isDirectory || name_matches_filter(name, filter)) filtered.push_back(e);
            }
            total = filtered.size();
            window = std::move(filtered);
            windowStart = 0;
        }
    }

    auto basename_sv = [](const std::string& s) -> std::string_view {
        const auto pos = s.find_last_of('/');
        if (pos == std::string::npos || pos + 1 >= s.size()) {
            return std::string_view{s};
        }
        return std::string_view{s}.substr(pos + 1);
    };
    auto clamp32 = [](std::uint64_t v) -> std::uint32_t {
        return v > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<std::uint32_t>(v);
    };

    std::string pages;
    std::uint8_t pageCount = 0;
    bool anyCut = false;
    bool firstPageDidNotFit = false;
    std::size_t nextIndex = firstIndex; // first entry not yet returned

    if (!totalOnly) {
        std::size_t budget = maxPayloadBytes;
        while (nextIndex < total) {
            const std::size_t inPage = std::min<std::size_t>(pageSize, total - nextIndex);
            if (pageCount > 0 && budget < kPageHeader + inPage * (kEntryFixed + 1)) {
                break; // cannot fit another page; avoid fetching it
            }
            // Encode the page into a scratch string so a page that does not fit is dropped whole.
            std::string page;
            bool pageCut = false;
            fileproto::write_u8(page, static_cast<std::uint8_t>(inPage));
            fileproto::write_u16le(page, 0); // dataLen placeholder
            for (std::size_t k = 0; k < inPage; ++k) {
                const std::size_t idx = nextIndex + k;
                if (idx < windowStart || idx - windowStart >= window.size()) {
                    // The window is sized from the budget; if it ran short, reload from here.
                    windowStart = idx;
                    std::size_t t = 0;
                    if (_dirCache.get_range(*fs, canonical_uri, resolvedPath, sortOrder, false, windowStart, want,
                                            window, t) != fs::ListStatus::Ok ||
                        window.empty()) {
                        resp.status = StatusCode::IOError;
                        resp.payload.clear();
                        return resp;
                    }
                }
                const FileInfo& e = window[idx - windowStart];
                const auto name = basename_sv(e.path);
                const std::size_t nameLen = std::min<std::size_t>(name.size(), maxNameBytes);
                std::uint8_t eflags = e.isDirectory ? lp::kEntryFlagDirectory : 0;
                if (nameLen < name.size()) {
                    eflags |= lp::kEntryFlagNameCut;
                    pageCut = true;
                }
                fileproto::write_u8(page, eflags);
                fileproto::write_u8(page, static_cast<std::uint8_t>(nameLen));
                fileproto::write_bytes(page, name.data(), nameLen);
                fileproto::write_u32le(page, e.isDirectory ? 0U : clamp32(e.sizeBytes));
                fileproto::write_u32le(page, clamp32(to_unix_seconds(e.modifiedTime)));
            }
            const std::size_t dataLen = page.size() - kPageHeader;
            if (page.size() > budget || pageCount == 0xFF) {
                if (pageCount == 0) {
                    firstPageDidNotFit = true;
                }
                break;
            }
            page[1] = static_cast<char>(dataLen & 0xFF);
            page[2] = static_cast<char>((dataLen >> 8) & 0xFF);
            budget -= page.size();
            pages.append(page);
            anyCut = anyCut || pageCut;
            ++pageCount;
            nextIndex += inPage;
        }
    }

    const bool more = totalOnly ? (firstIndex < total) : (firstPageDidNotFit || nextIndex < total);
    std::uint8_t flags = 0;
    if (more) flags |= lp::kResponseFlagMore;
    if (anyCut) flags |= lp::kResponseFlagNameCut;

    // Response: u8 version, u8 flags, u16 reserved, u32 total, u16 startPage, u8 pageSize,
    // u8 pageCount, pages...
    std::string out;
    out.reserve(11 + pages.size());
    fileproto::write_u8(out, FILEPROTO_VERSION);
    fileproto::write_u8(out, flags);
    fileproto::write_u16le(out, 0);
    fileproto::write_u32le(out, static_cast<std::uint32_t>(total));
    fileproto::write_u16le(out, startPage);
    fileproto::write_u8(out, pageSize);
    fileproto::write_u8(out, pageCount);
    out.append(pages);
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

    bool reused = false;
    auto file = open_for_read(*fs, resolvedPath, reused);
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

    std::size_t n = file->read(out.data() + dataStart, maxBytes);
    if (n == 0 && reused) {
        // A held handle can go stale (a TNFS server restarted, say): read once more from a
        // fresh open, so nothing is reported that a new open would not report.
        file.reset();
        file = fs->open(resolvedPath, "rb");
        if (!file || !file->seek(static_cast<std::uint64_t>(offset))) {
            resp.status = StatusCode::IOError;
            return resp;
        }
        n = file->read(out.data() + dataStart, maxBytes);
    }

    // Keep a network file open for the next read unless this one reached its end. A local
    // file is cheap to open, and the HotSync task may be writing it, so it is closed.
    if (n == maxBytes && fujinet::fs::is_network_kind(fs->kind())) {
        _heldRead.fs = fs;
        _heldRead.path = resolvedPath;
        _heldRead.file = std::move(file);
        _heldRead.lastUse = std::chrono::steady_clock::now();
    }

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

    // A file held open for reading could be this one, with blocks cached from before the write.
    _heldRead = HeldRead{};

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

} // namespace fujinet::io
