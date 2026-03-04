#include "fujinet/fs/path_resolvers/fs_prefix_resolver.h"

#include "fujinet/fs/path_resolvers/path_resolver_utils.h"

namespace fujinet::fs {

bool FsPrefixResolver::can_handle(std::string_view spec, const PathContext& /*ctx*/) const
{
    return spec.find(':') != std::string_view::npos;
}

bool FsPrefixResolver::resolve(std::string_view spec, const PathContext& /*ctx*/, ResolvedTarget& out) const
{
    const std::size_t colon = spec.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }

    out.fs_name = std::string(spec.substr(0, colon));
    std::string_view p = spec.substr(colon + 1);
    if (p.empty()) p = "/";

    if (!p.empty() && p.front() != '/') {
        std::string tmp("/");
        tmp.append(p.data(), p.size());
        out.fs_path = fs_norm(tmp);
    } else {
        out.fs_path = fs_norm(p);
    }

    out.display_path = out.fs_name + ":" + out.fs_path;
    return true;
}

} // namespace fujinet::fs
