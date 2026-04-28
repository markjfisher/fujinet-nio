#include "fujinet/fs/path_resolvers/http_relative_resolver.h"

#include "fujinet/fs/path_resolvers/http_path_utils.h"

namespace fujinet::fs {

bool HttpRelativeResolver::can_handle(std::string_view spec, const PathContext& ctx) const
{
    if (ctx.cwd_fs != "http") {
        return false;
    }
    return is_http_uri(ctx.cwd_path) || (!spec.empty() && is_http_uri(spec));
}

bool HttpRelativeResolver::resolve(std::string_view spec, const PathContext& ctx, ResolvedTarget& out) const
{
    if (!can_handle(spec, ctx)) {
        return false;
    }

    out.fs_name = "http";
    if (!spec.empty() && is_http_uri(spec)) {
        out.fs_path = std::string(spec);
    } else if (!spec.empty() && spec.front() == '/') {
        out.fs_path = http_replace_path(ctx.cwd_path, spec);
    } else {
        out.fs_path = http_join_relative(ctx.cwd_path, spec);
    }
    out.display_path = out.fs_name + ":" + out.fs_path;
    return true;
}

} // namespace fujinet::fs
