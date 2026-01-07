#pragma once

#include "fujinet/core/core.h"
#include "fujinet/io/devices/network_protocol_registry.h"

namespace fujinet::core {

void register_file_device(FujinetCore& core);
void register_clock_device(FujinetCore& core);
void register_network_device(FujinetCore& core);
void register_network_device(FujinetCore& core, io::ProtocolRegistry registry);
void register_disk_device(FujinetCore& core);

} // namespace fujinet::core
