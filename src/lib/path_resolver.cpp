#include "fujinet/fs/path_resolver.h"

#include "fujinet/fs/fs_prefix_resolver.h"
#include "fujinet/fs/relative_path_resolver.h"
#include "fujinet/fs/tnfs_prefix_resolver.h"
#include "fujinet/fs/tnfs_relative_resolver.h"
#include "fujinet/fs/tnfs_uri_resolver.h"

#include <utility>

namespace fujinet::fs {

PathResolver::PathResolver()
{
    registerHandler(std::make_unique<TnfsUriResolver>());
    registerHandler(std::make_unique<TnfsPrefixResolver>());
    registerHandler(std::make_unique<TnfsRelativeResolver>());
    registerHandler(std::make_unique<FsPrefixResolver>());
    registerHandler(std::make_unique<RelativePathResolver>());
}

bool PathResolver::registerHandler(std::unique_ptr<IPathHandler> handler)
{
    if (!handler) return false;
    _handlers.push_back(std::move(handler));
    return true;
}

bool PathResolver::resolve(std::string_view spec, const PathContext& ctx, ResolvedTarget& out) const
{
    for (const auto& handler : _handlers) {
        if (handler->can_handle(spec, ctx) && handler->resolve(spec, ctx, out)) {
            return true;
        }
    }
    return false;
}

bool PathResolver::resolveOrCwd(const std::vector<std::string_view>& argv, const PathContext& ctx, ResolvedTarget& out) const
{
    if (argv.size() >= 2) {
        return resolve(argv[1], ctx, out);
    }
    if (ctx.cwd_fs.empty()) {
        return false;
    }
    out.fs_name = ctx.cwd_fs;
    out.fs_path = ctx.cwd_path;
    out.display_path = out.fs_name + ":" + out.fs_path;
    return true;
}

} // namespace fujinet::fs
