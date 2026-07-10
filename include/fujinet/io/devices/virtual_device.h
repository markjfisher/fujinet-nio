#pragma once

#include "fujinet/io/core/io_message.h"

namespace fujinet::io {

// Abstract base for all addressable request handlers.
//
// Most handlers represent virtual devices (disk, printer, clock, modem, etc.).
// Some handlers are management services for FujiNet's own state. They still use
// the same transport routing and request lifecycle, but do not represent a
// device. Use VirtualService as a documentation alias for those handlers.
class VirtualDevice {
public:
    virtual ~VirtualDevice() = default;

    // Handle a single request from the host.
    virtual IOResponse handle(const IORequest& request) = 0;

    // Called periodically by IODeviceManager / IOService.
    // Devices that don't need polling can ignore this (default no-op).
    virtual void poll() {}
};

using VirtualService = VirtualDevice;

} // namespace fujinet::io
