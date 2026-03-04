#include "fujinet/fs/tnfs_path_utils.h"

#include "fujinet/fs/path_resolver_utils.h"

namespace fujinet::fs {

bool is_tnfs_uri(std::string_view spec)
{
    return spec.rfind("tnfs://", 0) == 0 ||
           spec.rfind("tnfs+tcp://", 0) == 0 ||
           spec.rfind("tnfstcp://", 0) == 0 ||
           spec.rfind("tnfs-tcp://", 0) == 0;
}

bool is_tnfs_endpoint_path(std::string_view p)
{
    return p.rfind("//", 0) == 0 || is_tnfs_uri(p);
}

std::string tnfs_join_relative(std::string_view base, std::string_view rel)
{
    std::size_t scheme_pos = base.find("://");
    if (scheme_pos != std::string_view::npos) {
        std::size_t authority_start = scheme_pos + 3;
        std::size_t path_start = base.find('/', authority_start);
        if (path_start == std::string_view::npos) {
            std::string out(base);
            out.push_back('/');
            out.append(rel.data(), rel.size());
            return out;
        }
        std::string prefix(base.substr(0, path_start));
        std::string path = fs_norm(fs_join(base.substr(path_start), rel));
        return prefix + path;
    }

    if (base.rfind("//", 0) == 0) {
        std::size_t path_start = base.find('/', 2);
        if (path_start == std::string_view::npos) {
            std::string out(base);
            out.push_back('/');
            out.append(rel.data(), rel.size());
            return out;
        }
        std::string prefix(base.substr(0, path_start));
        std::string path = fs_norm(fs_join(base.substr(path_start), rel));
        return prefix + path;
    }

    return fs_norm(fs_join(base, rel));
}

} // namespace fujinet::fs
