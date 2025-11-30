#pragma once

#include "fujinet/io/core/io_message.h"

namespace fujinet::io {

// Abstract base for all virtual devices (disk, printer, clock, modem, CP/M, etc.).
class VirtualDevice {
public:
    virtual ~VirtualDevice() = default;

    // Handle a single request from the host.
    virtual IOResponse handle(const IORequest& request) = 0;

    // Called periodically by IODeviceManager / IOService.
    // Devices that don't need polling can ignore this (default no-op).
    virtual void poll() {}
};

} // namespace fujinet::io
