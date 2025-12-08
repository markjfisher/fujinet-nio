#pragma once

#include <memory>
#include <string>

#include "fujinet/config/fuji_config.h"
#include "fujinet/fs/storage_manager.h"

namespace fujinet::platform {

std::unique_ptr<fujinet::config::FujiConfigStore>
create_fuji_config_store(fujinet::fs::StorageManager& storage);

} // namespace fujinet::platform
