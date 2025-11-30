#pragma once

#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/core/io_device_manager.h"

namespace fujinet::io {

// RoutingManager sits in front of IODeviceManager and can
// optionally delegate to a higher-priority "override" handler
// (e.g. CP/M mode, modem session, etc.).
//
// For now, it simply forwards all requests to IODeviceManager,
// but the API allows you to plug in overrides later.
class RoutingManager : public IRequestHandler {
public:
    explicit RoutingManager(IODeviceManager& deviceManager)
        : _deviceManager(deviceManager)
        , _overrideHandler(nullptr)
    {}

    // Set a global override handler. If non-null, all requests will be
    // routed to this handler instead of directly to IODeviceManager.
    //
    // In the future, this could be a CP/M handler, modem session handler, etc.
    void setOverrideHandler(IRequestHandler* handler) {
        _overrideHandler = handler;
    }

    // Remove any global override; subsequent requests go to IODeviceManager.
    void clearOverrideHandler() {
        _overrideHandler = nullptr;
    }

    bool hasOverrideHandler() const {
        return _overrideHandler != nullptr;
    }

    // IRequestHandler implementation.
    IOResponse handleRequest(const IORequest& request) override;

    // Access to the underlying device manager, in case you need to
    // register/unregister devices, or call pollDevices() from elsewhere.
    IODeviceManager&       deviceManager()       { return _deviceManager; }
    const IODeviceManager& deviceManager() const { return _deviceManager; }

private:
    IODeviceManager& _deviceManager;

    // Optional high-priority handler that can "take over" routing.
    // Not owned; lifetime is managed externally.
    IRequestHandler* _overrideHandler;
};

} // namespace fujinet::io
