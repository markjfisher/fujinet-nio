#include "fujinet/io/core/routing_manager.h"

namespace fujinet::io {

IOResponse RoutingManager::handleRequest(const IORequest& request)
{
    // If an override handler is installed, let it handle the request.
    // This is where, in the future, CP/M or modem "takeover" logic
    // will plug in.
    if (_overrideHandler) {
        return _overrideHandler->handleRequest(request);
    }

    // Default behavior: just route to the device manager.
    return _deviceManager.handleRequest(request);
}

} // namespace fujinet::io
