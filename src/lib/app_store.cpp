#include "fujinet/io/devices/app_store.h"

#include "fujinet/fs/path_resolvers/path_resolver.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace fujinet::io {

namespace {

constexpr const char* kRoot = "/FujiNet/app-store/v1";
constexpr const char* kHostNamespace = "fujinet-nio";
constexpr const char* kCurrentHostKey = "current-host";
constexpr const char* kCurrentDisplayPathKey = "current-display-path";
constexpr const char* kHostHistoryKey = "host-history";
constexpr std::size_t kHostHistoryMax = 32;

std::uint64_t to_unix_seconds(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{}) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count());
}

bool is_unreserved(unsigned char c)
{
    return std::isalnum(c) || c == '-' || c == '_' || c == '.';
}

std::string encode_segment(std::string_view input)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        if (is_unreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

bool decode_segment(std::string_view input, std::string& out)
{
    out.clear();
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '%') {
            out.push_back(input[i]);
            continue;
        }
        if (i + 2 >= input.size()) {
            return false;
        }
        const int hi = hex_value(input[i + 1]);
        const int lo = hex_value(input[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

std::string basename(const std::string& path)
{
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

std::string strip_suffix(std::string s, std::string_view suffix)
{
    if (s.size() >= suffix.size() &&
        std::string_view{s}.substr(s.size() - suffix.size()) == suffix) {
        s.resize(s.size() - suffix.size());
    }
    return s;
}

bool mkdir_parents(fujinet::fs::IFileSystem& fs, const std::string& path)
{
    if (path.empty() || path == "/") {
        return true;
    }
    std::string cur;
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        if (i >= path.size()) break;
        const std::size_t j = path.find('/', i);
        const auto end = (j == std::string::npos) ? path.size() : j;
        cur += "/";
        cur += path.substr(i, end - i);
        if (!fs.createDirectory(cur) && !fs.isDirectory(cur)) {
            return false;
        }
        i = end;
    }
    return true;
}

std::string build_resolved_uri(const fs::ResolvedTarget& target)
{
    if (target.fs_path.find("://") != std::string::npos) {
        return target.fs_path;
    }
    return target.fs_name + ":" + target.fs_path;
}

std::string build_display_path(const fs::ResolvedTarget& target)
{
    std::string path = target.fs_path;

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

bool make_path_context(std::string_view baseUri, fs::PathContext& ctx)
{
    fs::PathResolver resolver;
    fs::ResolvedTarget target;
    if (!resolver.resolve(baseUri, {}, target)) {
        return false;
    }
    ctx.cwd_fs = target.fs_name;
    ctx.cwd_path = target.fs_path;
    return true;
}

bool must_resolve_against_current(std::string_view spec)
{
    return spec.empty() || spec.front() == '/';
}

} // namespace

AppStore::AppStore(fs::StorageManager& storage)
    : _storage(storage)
{}

fs::IFileSystem* AppStore::backing_fs() const
{
    return _storage.defaultPersistentFileSystem();
}

bool AppStore::available() const
{
    return backing_fs() != nullptr;
}

std::string AppStore::backing_fs_name() const
{
    auto* fs = backing_fs();
    return fs ? fs->name() : std::string{};
}

bool AppStore::valid_namespace(std::string_view ns)
{
    return !ns.empty() && ns.size() <= 255;
}

bool AppStore::valid_key(std::string_view key)
{
    return !key.empty() && key.size() <= 255;
}

std::string AppStore::namespace_path(std::string_view ns) const
{
    return std::string{kRoot} + "/" + encode_segment(ns);
}

std::string AppStore::key_path(std::string_view ns, std::string_view key) const
{
    return namespace_path(ns) + "/" + encode_segment(key) + ".bin";
}

bool AppStore::ensure_namespace_dir(std::string_view ns)
{
    auto* fs = backing_fs();
    if (!fs) return false;
    return mkdir_parents(*fs, namespace_path(ns));
}

bool AppStore::stat(std::string_view ns, std::string_view key, Stat& out)
{
    out = {};
    if (!valid_namespace(ns) || !valid_key(key)) return false;
    auto* fs = backing_fs();
    if (!fs) return false;

    fs::FileInfo info{};
    if (!fs->stat(key_path(ns, key), info) || info.isDirectory) {
        return true;
    }
    out.exists = true;
    out.sizeBytes = info.sizeBytes;
    out.modifiedUnixTime = to_unix_seconds(info.modifiedTime);
    return true;
}

bool AppStore::read(std::string_view ns, std::string_view key, std::uint32_t offset, std::uint16_t maxBytes, ReadResult& out)
{
    out = {};
    out.offset = offset;
    if (!valid_namespace(ns) || !valid_key(key) || maxBytes == 0) return false;
    auto* fs = backing_fs();
    if (!fs) return false;

    Stat st{};
    if (!stat(ns, key, st)) return false;
    if (!st.exists) {
        out.exists = false;
        out.eof = true;
        return true;
    }

    auto file = fs->open(key_path(ns, key), "rb");
    if (!file) return false;
    if (!file->seek(offset)) return false;

    out.exists = true;
    out.data.resize(maxBytes);
    const std::size_t n = file->read(out.data.data(), maxBytes);
    out.data.resize(n);
    out.eof = (static_cast<std::uint64_t>(offset) + n) >= st.sizeBytes || n < maxBytes;
    return true;
}

bool AppStore::write(std::string_view ns, std::string_view key, std::uint32_t offset, const std::uint8_t* data, std::uint16_t len, WriteResult& out)
{
    if (ns == kHostNamespace && key == kCurrentHostKey) {
        out = {};
        out.offset = offset;
        if (offset != 0 || data == nullptr) return false;
        const std::string spec(reinterpret_cast<const char*>(data), len);
        if (!set_current_host(spec)) return false;
        out.written = len;
        return true;
    }

    return raw_write(ns, key, offset, data, len, out);
}

bool AppStore::raw_write(std::string_view ns, std::string_view key, std::uint32_t offset, const std::uint8_t* data, std::uint16_t len, WriteResult& out)
{
    out = {};
    out.offset = offset;
    if (!valid_namespace(ns) || !valid_key(key)) return false;
    auto* fs = backing_fs();
    if (!fs) return false;
    if (!ensure_namespace_dir(ns)) return false;

    const char* mode = (offset == 0) ? "wb" : "r+b";
    auto file = fs->open(key_path(ns, key), mode);
    if (!file && offset > 0) {
        file = fs->open(key_path(ns, key), "wb");
    }
    if (!file) return false;
    if (offset > 0 && !file->seek(offset)) return false;
    const std::size_t written = file->write(data, len);
    (void)file->flush();
    out.written = static_cast<std::uint16_t>(std::min<std::size_t>(written, 0xFFFF));
    return true;
}

bool AppStore::get_current_host(std::string* uri, std::string* displayPath)
{
    if (!uri) return false;
    uri->clear();
    if (displayPath) displayPath->clear();

    ReadResult rr{};
    if (!read(kHostNamespace, kCurrentHostKey, 0, 512, rr) || !rr.exists) {
        return false;
    }
    uri->assign(reinterpret_cast<const char*>(rr.data.data()), rr.data.size());
    if (uri->empty()) return false;

    if (displayPath) {
        ReadResult pr{};
        if (read(kHostNamespace, kCurrentDisplayPathKey, 0, 512, pr) && pr.exists) {
            displayPath->assign(reinterpret_cast<const char*>(pr.data.data()), pr.data.size());
        }
    }
    return true;
}

bool AppStore::resolve_target(std::string_view spec, std::string& uri, std::string* displayPath)
{
    fs::PathResolver resolver;
    fs::ResolvedTarget target;

    if (!must_resolve_against_current(spec) && resolver.resolve(spec, {}, target)) {
        uri = build_resolved_uri(target);
        if (displayPath) *displayPath = build_display_path(target);
        return true;
    }

    std::string current;
    if (!get_current_host(&current, nullptr)) {
        return false;
    }

    fs::PathContext ctx;
    if (!make_path_context(current, ctx) || !resolver.resolve(spec, ctx, target)) {
        return false;
    }

    uri = build_resolved_uri(target);
    if (displayPath) *displayPath = build_display_path(target);
    return true;
}

bool AppStore::set_current_host(std::string_view spec)
{
    std::string uri;
    std::string displayPath;
    if (!resolve_target(spec, uri, &displayPath)) {
        return false;
    }

    auto [fs, resolvedPath] = _storage.resolveUri(uri);
    if (!fs || !fs->isDirectory(resolvedPath)) {
        return false;
    }

    WriteResult wr{};
    if (!raw_write(kHostNamespace, kCurrentHostKey, 0,
                   reinterpret_cast<const std::uint8_t*>(uri.data()),
                   static_cast<std::uint16_t>(uri.size()), wr) ||
        wr.written != uri.size()) {
        return false;
    }

    if (!raw_write(kHostNamespace, kCurrentDisplayPathKey, 0,
                   reinterpret_cast<const std::uint8_t*>(displayPath.data()),
                   static_cast<std::uint16_t>(displayPath.size()), wr) ||
        wr.written != displayPath.size()) {
        return false;
    }

    return update_host_history(uri);
}

bool AppStore::update_host_history(std::string_view uri)
{
    std::vector<std::string> entries;
    ReadResult rr{};
    if (read(kHostNamespace, kHostHistoryKey, 0, 8192, rr) && rr.exists) {
        const std::string existing(reinterpret_cast<const char*>(rr.data.data()), rr.data.size());
        std::istringstream input(existing);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line != uri) {
                entries.push_back(std::move(line));
            }
        }
    }

    entries.insert(entries.begin(), std::string(uri));
    if (entries.size() > kHostHistoryMax) {
        entries.resize(kHostHistoryMax);
    }

    std::string text;
    for (const auto& entry : entries) {
        text += entry;
        text += '\n';
    }

    WriteResult wr{};
    return raw_write(kHostNamespace, kHostHistoryKey, 0,
                     reinterpret_cast<const std::uint8_t*>(text.data()),
                     static_cast<std::uint16_t>(text.size()), wr) &&
           wr.written == text.size();
}

bool AppStore::remove(std::string_view ns, std::string_view key, DeleteResult& out)
{
    out = {};
    if (!valid_namespace(ns) || !valid_key(key)) return false;
    auto* fs = backing_fs();
    if (!fs) return false;
    const std::string path = key_path(ns, key);
    if (!fs->exists(path)) {
        return true;
    }
    out.deleted = fs->removeFile(path);
    return out.deleted;
}

bool AppStore::list(std::string_view ns, std::uint16_t startIndex, std::uint16_t maxPayloadBytes, ListResult& out)
{
    out = {};
    out.startIndex = startIndex;
    if (!valid_namespace(ns) || maxPayloadBytes == 0) return false;
    auto* fs = backing_fs();
    if (!fs) return false;

    const std::string dir = namespace_path(ns);
    if (!fs->isDirectory(dir)) {
        return true;
    }

    std::vector<fs::FileInfo> entries;
    if (!fs->listDirectory(dir, entries)) return false;

    std::vector<std::string> keys;
    for (const auto& entry : entries) {
        if (entry.isDirectory) continue;
        std::string encoded = strip_suffix(basename(entry.path), ".bin");
        std::string decoded;
        if (decode_segment(encoded, decoded)) {
            keys.push_back(std::move(decoded));
        }
    }
    std::sort(keys.begin(), keys.end());

    std::size_t used = 0;
    const std::size_t start = std::min<std::size_t>(startIndex, keys.size());
    for (std::size_t i = start; i < keys.size(); ++i) {
        const std::size_t bytes = 2 + keys[i].size();
        if (used + bytes > maxPayloadBytes) break;
        used += bytes;
        out.keys.push_back(keys[i]);
    }
    out.more = (start + out.keys.size()) < keys.size();
    return true;
}

bool AppStore::rename(std::string_view ns, std::string_view oldKey, std::string_view newKey)
{
    if (!valid_namespace(ns) || !valid_key(oldKey) || !valid_key(newKey)) return false;
    auto* fs = backing_fs();
    if (!fs) return false;
    if (!ensure_namespace_dir(ns)) return false;
    return fs->rename(key_path(ns, oldKey), key_path(ns, newKey));
}

} // namespace fujinet::io
