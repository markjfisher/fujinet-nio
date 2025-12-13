#pragma once

#include "fujinet/io/devices/virtual_device.h"

namespace fujinet::io {

class ClockDevice : public VirtualDevice {
public:
    IOResponse handle(const IORequest& request) override;
    void poll() override {}
};

} // namespace fujinet::io
