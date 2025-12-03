#pragma once

#include <memory>
#include <string>

#include "fujinet/core/fuji_config.h"

namespace fujinet::platform {

std::unique_ptr<fujinet::core::FujiConfigStore>
create_fuji_config_store(const std::string& rootHint = {});

} // namespace fujinet::platform
