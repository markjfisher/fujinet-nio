#pragma once

#include "fujinet/io/devices/network_protocol_registry.h"

namespace fujinet::platform {

// Build a default URL-scheme -> protocol backend registry for the current platform.
// Implemented in platform-specific .cpp files (POSIX / ESP32).
io::ProtocolRegistry make_default_network_registry();

} // namespace fujinet::platform


