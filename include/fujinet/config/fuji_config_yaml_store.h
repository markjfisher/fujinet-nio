#pragma once

#include <string>

#include "fujinet/config/fuji_config.h"

namespace fujinet::config {

// File-backed YAML implementation of FujiConfigStore.
class YamlFujiConfigStore : public FujiConfigStore {
public:
    explicit YamlFujiConfigStore(std::string path);

    FujiConfig load() override;
    void       save(const FujiConfig& cfg) override;

private:
    std::string _path;
};

} // namespace fujinet::config
