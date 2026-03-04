#pragma once

#include "fujinet/fs/path_resolvers/path_resolver.h"

namespace fujinet::fs {

class TnfsUriResolver final : public IPathHandler {
public:
    bool can_handle(std::string_view spec, const PathContext& ctx) const override;
    bool resolve(std::string_view spec, const PathContext& ctx, ResolvedTarget& out) const override;
};

} // namespace fujinet::fs
