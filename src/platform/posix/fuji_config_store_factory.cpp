// src/platform/posix/fuji_config_store_factory.cpp

#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/core/logging.h"

namespace fujinet::platform {

using fujinet::log::Level;
static constexpr const char* TAG = "config_factory";

std::unique_ptr<config::FujiConfigStore>
create_fuji_config_store(fs::StorageManager& storage)
{
    auto* host = storage.get("host");

    if (!host) {
        FN_LOGE(TAG, "No 'host' filesystem registered; config will stay in-memory");
        return std::make_unique<config::YamlFujiConfigStoreFs>(
            nullptr, nullptr, "fujinet.yaml"
        );
    }

    return std::make_unique<config::YamlFujiConfigStoreFs>(
        host, nullptr, "fujinet.yaml"
    );
}

} // namespace fujinet::platform
