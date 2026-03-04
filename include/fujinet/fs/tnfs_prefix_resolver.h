#pragma once

#include "fujinet/fs/path_resolver.h"

namespace fujinet::fs {

class TnfsPrefixResolver final : public IPathHandler {
public:
    bool can_handle(std::string_view spec, const PathContext& ctx) const override;
    bool resolve(std::string_view spec, const PathContext& ctx, ResolvedTarget& out) const override;
};

} // namespace fujinet::fs
