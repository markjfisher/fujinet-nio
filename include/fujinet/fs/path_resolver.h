#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::fs {

struct PathContext {
    std::string cwd_fs;
    std::string cwd_path;
};

struct ResolvedTarget {
    std::string fs_name;
    std::string fs_path;
    std::string display_path;
};

class IPathHandler {
public:
    virtual ~IPathHandler() = default;
    virtual bool can_handle(std::string_view spec, const PathContext& ctx) const = 0;
    virtual bool resolve(std::string_view spec, const PathContext& ctx, ResolvedTarget& out) const = 0;
};

class PathResolver {
public:
    PathResolver();
    ~PathResolver() = default;

    PathResolver(const PathResolver&) = delete;
    PathResolver& operator=(const PathResolver&) = delete;

    bool registerHandler(std::unique_ptr<IPathHandler> handler);

    bool resolve(std::string_view spec, const PathContext& ctx, ResolvedTarget& out) const;
    bool resolveOrCwd(const std::vector<std::string_view>& argv, const PathContext& ctx, ResolvedTarget& out) const;

private:
    std::vector<std::unique_ptr<IPathHandler>> _handlers;
};

} // namespace fujinet::fs
