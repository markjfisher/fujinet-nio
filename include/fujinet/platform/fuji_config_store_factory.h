#pragma once

#include <memory>
#include <string>

#include "fujinet/config/fuji_config.h"

namespace fujinet::platform {

std::unique_ptr<fujinet::config::FujiConfigStore>
create_fuji_config_store(const std::string& rootHint = {});

} // namespace fujinet::platform
