#pragma once

#include "fujinet/core/system_events.h"

namespace fujinet::platform::posix {

/**
 * POSIX "SNTP service":
 * - typically no-op (system time already managed by OS)
 * - kept to match ESP32 lifecycle + future extensibility
 */
class SntpService {
public:
    explicit SntpService(fujinet::core::SystemEvents& /*events*/) {}
    ~SntpService() = default;
};

} // namespace fujinet::platform::posix
