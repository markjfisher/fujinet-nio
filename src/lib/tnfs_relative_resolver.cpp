#include "fujinet/fs/tnfs_relative_resolver.h"

#include "fujinet/fs/path_resolver_utils.h"
#include "fujinet/fs/tnfs_path_utils.h"

namespace fujinet::fs {

bool TnfsRelativeResolver::can_handle(std::string_view spec, const PathContext& ctx) const
{
    if (ctx.cwd_fs != "tnfs") {
        return false;
    }
    return is_tnfs_endpoint_path(ctx.cwd_path) || (!spec.empty() && is_tnfs_endpoint_path(spec));
}

bool TnfsRelativeResolver::resolve(std::string_view spec, const PathContext& ctx, ResolvedTarget& out) const
{
    if (!can_handle(spec, ctx)) {
        return false;
    }

    out.fs_name = "tnfs";
    if (!spec.empty() && spec.front() == '/') {
        if (is_tnfs_endpoint_path(spec)) {
            out.fs_path = std::string(spec);
        } else {
            out.fs_path = fs_norm(spec);
        }
    } else {
        out.fs_path = tnfs_join_relative(ctx.cwd_path, spec);
    }
    out.display_path = out.fs_name + ":" + out.fs_path;
    return true;
}

} // namespace fujinet::fs
