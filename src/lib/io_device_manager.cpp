#include "fujinet/io/core/io_device_manager.h"

namespace fujinet::io {

bool IODeviceManager::registerDevice(DeviceID id, std::unique_ptr<VirtualDevice> device)
{
    if (!device) {
        return false;
    }

    auto [it, inserted] = _devices.emplace(id, std::move(device));
    return inserted;
}

bool IODeviceManager::unregisterDevice(DeviceID id)
{
    return _devices.erase(id) > 0;
}

VirtualDevice* IODeviceManager::getDevice(DeviceID id)
{
    auto it = _devices.find(id);
    return (it == _devices.end()) ? nullptr : it->second.get();
}

const VirtualDevice* IODeviceManager::getDevice(DeviceID id) const
{
    auto it = _devices.find(id);
    return (it == _devices.end()) ? nullptr : it->second.get();
}

IOResponse IODeviceManager::handleRequest(const IORequest& request)
{
    IOResponse response;
    response.id       = request.id;
    response.deviceId = request.deviceId;

    auto* device = getDevice(request.deviceId);
    if (!device) {
        response.status  = StatusCode::DeviceNotFound;
        response.payload = {};
        return response;
    }

    // Delegate to the device.
    // Devices are responsible for setting status and payload.
    IOResponse devResp = device->handle(request);

    // Ensure the device didn't accidentally change the correlation fields.
    devResp.id         = request.id;
    devResp.deviceId   = request.deviceId;

    return devResp;
}

void IODeviceManager::pollDevices()
{
    for (auto& [id, dev] : _devices) {
        (void)id;
        if (dev) {
            dev->poll();
        }
    }
}

} // namespace fujinet::io
