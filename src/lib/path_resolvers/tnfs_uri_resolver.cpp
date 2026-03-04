#include "fujinet/fs/tnfs_uri_resolver.h"

#include "fujinet/fs/tnfs_path_utils.h"

namespace fujinet::fs {

bool TnfsUriResolver::can_handle(std::string_view spec, const PathContext& /*ctx*/) const
{
    return is_tnfs_uri(spec);
}

bool TnfsUriResolver::resolve(std::string_view spec, const PathContext& /*ctx*/, ResolvedTarget& out) const
{
    out.fs_name = "tnfs";
    out.fs_path = std::string(spec);
    out.display_path = out.fs_name + ":" + out.fs_path;
    return true;
}

} // namespace fujinet::fs
