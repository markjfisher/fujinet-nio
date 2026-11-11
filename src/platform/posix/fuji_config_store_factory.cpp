// src/platform/posix/fuji_config_store_factory.cpp

#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config_defaults.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/core/logging.h"

namespace fujinet::platform {

using fujinet::log::Level;
static constexpr const char* TAG = "platform";

std::unique_ptr<config::FujiConfigStore>
create_fuji_config_store(fs::StorageManager& storage)
{
    // What a fresh device starts from: the profile knows its platform's boot disk.
    const auto profile = build::current_build_profile();
    const auto defaults = config::default_config_for(profile);

    auto* host = storage.get("host");
    const bool emitVisorDefaults = profile.primaryChannel == build::ChannelKind::VisorUsbHost;

    if (!host) {
        FN_LOGE(TAG, "No 'host' filesystem registered; config will stay in-memory");
        return std::make_unique<config::YamlFujiConfigStoreFs>(
            nullptr, nullptr, "fujinet.yaml", defaults, emitVisorDefaults
        );
    }

    return std::make_unique<config::YamlFujiConfigStoreFs>(
        host, nullptr, "fujinet.yaml", defaults, emitVisorDefaults
    );
}

} // namespace fujinet::platform
