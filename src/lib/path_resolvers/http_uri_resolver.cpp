#include "fujinet/fs/path_resolvers/http_uri_resolver.h"

#include "fujinet/fs/path_resolvers/http_path_utils.h"

namespace fujinet::fs {

bool HttpUriResolver::can_handle(std::string_view spec, const PathContext& /*ctx*/) const
{
    return is_http_uri(spec);
}

bool HttpUriResolver::resolve(std::string_view spec, const PathContext& /*ctx*/, ResolvedTarget& out) const
{
    out.fs_name = "http";
    out.fs_path = std::string(spec);
    out.display_path = out.fs_name + ":" + out.fs_path;
    return true;
}

} // namespace fujinet::fs
