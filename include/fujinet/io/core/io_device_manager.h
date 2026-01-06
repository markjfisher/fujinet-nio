#pragma once

#include <memory>
#include <unordered_map>

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/devices/virtual_device.h"

namespace fujinet::io {

// Owns and routes to VirtualDevice instances.
class IODeviceManager : public IRequestHandler {
public:
    IODeviceManager() = default;

    // Non-copyable (device ownership is unique).
    IODeviceManager(const IODeviceManager&) = delete;
    IODeviceManager& operator=(const IODeviceManager&) = delete;

    // Register a device for a given DeviceID.
    // Returns false if a device is already registered for that ID.
    bool registerDevice(DeviceID id, std::unique_ptr<VirtualDevice> device);

    // Remove a device by ID.
    // Returns true if a device was removed.
    bool unregisterDevice(DeviceID id);

    // Look up a device by ID. Returns nullptr if not found.
    VirtualDevice*       getDevice(DeviceID id);
    const VirtualDevice* getDevice(DeviceID id) const;

    // IRequestHandler implementation.
    IOResponse handleRequest(const IORequest& request) override;

    // Called periodically by higher-level code (e.g. IOService)
    // to let devices do background work.
    void pollDevices();

    std::size_t device_count() const noexcept { return _devices.size(); }

private:
    std::unordered_map<DeviceID, std::unique_ptr<VirtualDevice>> _devices;
};

} // namespace fujinet::io
