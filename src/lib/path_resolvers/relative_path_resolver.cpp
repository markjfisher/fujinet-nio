#include "fujinet/fs/path_resolvers/relative_path_resolver.h"

#include "fujinet/fs/path_resolvers/path_resolver_utils.h"

namespace fujinet::fs {

bool RelativePathResolver::can_handle(std::string_view /*spec*/, const PathContext& /*ctx*/) const
{
    return true;
}

bool RelativePathResolver::resolve(std::string_view spec, const PathContext& ctx, ResolvedTarget& out) const
{
    if (ctx.cwd_fs.empty()) {
        return false;
    }

    out.fs_name = ctx.cwd_fs;
    if (!spec.empty() && spec.front() == '/') {
        out.fs_path = fs_norm(spec);
    } else {
        out.fs_path = fs_norm(fs_join(ctx.cwd_path, spec));
    }

    out.display_path = out.fs_name + ":" + out.fs_path;
    return true;
}

} // namespace fujinet::fs
