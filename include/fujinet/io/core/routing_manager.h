#pragma once

#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/core/io_device_manager.h"

#include <memory>

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
        _overrideOwned.reset();
        _overrideHandler = handler;
    }

    // Set an override handler and transfer ownership to the RoutingManager.
    // This is useful for feature-gated handlers whose lifetime should match the core.
    void setOverrideHandler(std::unique_ptr<IRequestHandler> handler) {
        _overrideOwned = std::move(handler);
        _overrideHandler = _overrideOwned.get();
    }

    // Remove any global override; subsequent requests go to IODeviceManager.
    void clearOverrideHandler() {
        _overrideOwned.reset();
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

    // Optional owned override handler (used when installed via unique_ptr overload).
    std::unique_ptr<IRequestHandler> _overrideOwned;
};

} // namespace fujinet::io
