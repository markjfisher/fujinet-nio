#include "fujinet/io/host_state.h"

#include "fujinet/fs/path_resolvers/path_resolver.h"
#include "fujinet/io/devices/app_store.h"

#include <sstream>
#include <vector>

namespace fujinet::io {

namespace {

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

HostState::HostState(fs::StorageManager& storage)
    : _storage(storage)
{}

bool HostState::get_current_host(std::string* uri, std::string* displayPath)
{
    if (!uri) return false;
    uri->clear();
    if (displayPath) displayPath->clear();

    AppStore store(_storage);
    AppStore::ReadResult rr{};
    if (!store.read(kNamespace, kCurrentHostKey, 0, 512, rr) || !rr.exists) {
        return false;
    }
    uri->assign(reinterpret_cast<const char*>(rr.data.data()), rr.data.size());
    if (uri->empty()) return false;

    if (displayPath) {
        AppStore::ReadResult pr{};
        if (store.read(kNamespace, kCurrentDisplayPathKey, 0, 512, pr) && pr.exists) {
            displayPath->assign(reinterpret_cast<const char*>(pr.data.data()), pr.data.size());
        }
    }
    return true;
}

bool HostState::resolve_target(std::string_view spec, std::string& uri, std::string* displayPath)
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

bool HostState::set_current_host(std::string_view spec)
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

    return write_value(kCurrentHostKey, uri) &&
           write_value(kCurrentDisplayPathKey, displayPath) &&
           update_history(uri);
}

bool HostState::write_value(std::string_view key, std::string_view value)
{
    AppStore store(_storage);
    AppStore::WriteResult wr{};
    return store.write(kNamespace, key, 0,
                       reinterpret_cast<const std::uint8_t*>(value.data()),
                       static_cast<std::uint16_t>(value.size()), wr) &&
           wr.written == value.size();
}

bool HostState::update_history(std::string_view uri)
{
    std::vector<std::string> entries;
    AppStore store(_storage);
    AppStore::ReadResult rr{};
    if (store.read(kNamespace, kHostHistoryKey, 0, 8192, rr) && rr.exists) {
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

    return write_value(kHostHistoryKey, text);
}

} // namespace fujinet::io
