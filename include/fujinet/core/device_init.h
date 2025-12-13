#pragma once

#include "fujinet/core/core.h"

namespace fujinet::core {

void register_file_device(FujinetCore& core);
void register_clock_device(FujinetCore& core);
void register_network_device(FujinetCore& core);

} // namespace fujinet::core
