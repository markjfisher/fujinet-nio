#include "fujinet/fs/tnfs_prefix_resolver.h"

#include "fujinet/fs/tnfs_path_utils.h"

namespace fujinet::fs {

bool TnfsPrefixResolver::can_handle(std::string_view spec, const PathContext& /*ctx*/) const
{
    if (spec.rfind("tnfs:", 0) != 0) {
        return false;
    }
    std::string_view p = spec.substr(5);
    return is_tnfs_endpoint_path(p);
}

bool TnfsPrefixResolver::resolve(std::string_view spec, const PathContext& /*ctx*/, ResolvedTarget& out) const
{
    if (spec.rfind("tnfs:", 0) != 0) {
        return false;
    }
    std::string_view p = spec.substr(5);
    if (!is_tnfs_endpoint_path(p)) {
        return false;
    }

    out.fs_name = "tnfs";
    out.fs_path = std::string(p);
    out.display_path = out.fs_name + ":" + out.fs_path;
    return true;
}

} // namespace fujinet::fs
