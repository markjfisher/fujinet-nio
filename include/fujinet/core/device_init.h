#pragma once

#include "fujinet/config/fuji_config.h"
#include "fujinet/core/core.h"
#include "fujinet/io/devices/network_protocol_registry.h"

namespace fujinet::core {

void register_file_device(FujinetCore& core);

/// Register clock device without config persistence
void register_clock_device(FujinetCore& core);

/// Register clock device with config store for persistence
/// @param core The FujinetCore instance
/// @param configStore Non-owning pointer to config store for timezone persistence
void register_clock_device(FujinetCore& core, config::FujiConfigStore* configStore);

void register_network_device(FujinetCore& core);
void register_network_device(FujinetCore& core, io::ProtocolRegistry registry);
void register_disk_device(FujinetCore& core);
void register_modem_device(FujinetCore& core);

} // namespace fujinet::core
