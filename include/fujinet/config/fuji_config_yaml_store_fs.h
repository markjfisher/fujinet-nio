#pragma once

#include <string>

#include "fujinet/config/fuji_config.h"
#include "fujinet/fs/filesystem.h"

namespace fujinet::config {

class YamlFujiConfigStoreFs : public FujiConfigStore {
public:
    // primary can be null (no SD), backup can be null (no flash) – we handle both.
    YamlFujiConfigStoreFs(fs::IFileSystem* primary,
                          fs::IFileSystem* backup,
                          std::string      relativePath);

    FujiConfig load() override;
    void save(const FujiConfig& cfg) override;

private:
    fs::IFileSystem* _primary;
    fs::IFileSystem* _backup;
    std::string      _relPath; // e.g. "fujinet.yaml"

    FujiConfig loadFromFs(fs::IFileSystem& fs);
    void saveToFs(fs::IFileSystem& fs, const FujiConfig& cfg);
};

} // namespace fujinet::config
