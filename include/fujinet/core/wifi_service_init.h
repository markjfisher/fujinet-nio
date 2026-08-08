#pragma once

#include <functional>

#include "fujinet/config/fuji_config.h"
#include "fujinet/core/core.h"
#include "fujinet/net/network_link.h"

namespace fujinet::core {

void register_wifi_service(FujinetCore& core,
                           config::FujiConfig& config,
                           config::FujiConfigStore* store,
                           std::function<net::INetworkLink*()> linkProvider);

} // namespace fujinet::core
