#pragma once

#include <string>

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/devices/virtual_device.h"

namespace fujinet::io {

class ClockDevice : public VirtualDevice {
public:
    /// Default constructor (no config persistence)
    ClockDevice() = default;
    
    /// Constructor with config store for persistence
    /// @param configStore Non-owning pointer to config store for loading/saving timezone
    explicit ClockDevice(config::FujiConfigStore* configStore);

    IOResponse handle(const IORequest& request) override;
    void poll() override {}

private:
    config::FujiConfigStore* _configStore = nullptr;
};

} // namespace fujinet::io
