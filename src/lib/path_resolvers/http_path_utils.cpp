#include "fujinet/fs/path_resolvers/http_path_utils.h"

#include "fujinet/fs/path_resolvers/path_resolver_utils.h"

#include <cctype>

namespace fujinet::fs {

namespace {

std::string to_lower_ascii(std::string_view spec)
{
    std::string lower;
    lower.reserve(spec.size());
    for (char ch : spec) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lower;
}

bool split_http_uri(std::string_view uri, std::string& prefix, std::string& path)
{
    const std::size_t schemePos = uri.find("://");
    if (schemePos == std::string_view::npos) {
        return false;
    }

    const std::size_t authorityStart = schemePos + 3;
    const std::size_t pathStart = uri.find('/', authorityStart);
    if (pathStart == std::string_view::npos) {
        prefix.assign(uri.data(), uri.size());
        path = "/";
        return true;
    }

    prefix.assign(uri.substr(0, pathStart));
    path.assign(uri.substr(pathStart));
    return true;
}

} // namespace

bool is_http_uri(std::string_view spec)
{
    const std::string lower = to_lower_ascii(spec);
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

std::string http_join_relative(std::string_view base, std::string_view rel)
{
    std::string prefix;
    std::string path;
    if (!split_http_uri(base, prefix, path)) {
        return fs_norm(fs_join(base, rel));
    }

    return prefix + fs_norm(fs_join(path, rel));
}

std::string http_replace_path(std::string_view base, std::string_view absolutePath)
{
    std::string prefix;
    std::string path;
    if (!split_http_uri(base, prefix, path)) {
        return fs_norm(absolutePath);
    }

    return prefix + fs_norm(absolutePath);
}

} // namespace fujinet::fs
