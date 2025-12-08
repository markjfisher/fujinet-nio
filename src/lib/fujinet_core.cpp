#include "fujinet/core/core.h"

namespace fujinet::core {

FujinetCore::FujinetCore()
    : _deviceManager()
    , _routing(_deviceManager)
    , _ioService(_routing)
    , _storageManager()
{
}

void FujinetCore::tick()
{
    // 1. Let transports process I/O.
    _ioService.serviceOnce();

    // 2. Let devices do background work.
    _deviceManager.pollDevices();

    // 3. Increment tick counter for diagnostics.
    ++_tickCount;
}

void FujinetCore::addTransport(io::ITransport* transport)
{
    _ioService.addTransport(transport);
}

} // namespace fujinet::core
