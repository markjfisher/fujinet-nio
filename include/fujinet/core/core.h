#pragma once

#include <cstdint>

#include "fujinet/io/core/io_device_manager.h"
#include "fujinet/io/core/routing_manager.h"
#include "fujinet/io/transport/io_service.h"
#include "fujinet/fs/storage_manager.h"

namespace fujinet::core {

// Central engine for FujiNet I/O.
// Owns:
//   - IODeviceManager (devices)
//   - RoutingManager  (modes / overrides)
//   - IOService       (transports)
class FujinetCore {
public:
    FujinetCore();

    // One iteration of the core loop.
    void tick();

    // How many ticks have been executed so far.
    std::uint64_t tick_count() const noexcept { return _tickCount; }

    // Access to core subsystems for setup/registration.
    io::IODeviceManager&       deviceManager()        { return _deviceManager; }
    const io::IODeviceManager& deviceManager()  const { return _deviceManager; }

    io::RoutingManager&        routingManager()       { return _routing; }
    const io::RoutingManager&  routingManager() const { return _routing; }

    io::IOService&             ioService()            { return _ioService; }
    const io::IOService&       ioService()      const { return _ioService; }

    fs::StorageManager&        storageManager()       { return _storageManager; }
    const fs::StorageManager&  storageManager() const { return _storageManager; }

    // Helper to add transports to the IOService.
    void addTransport(io::ITransport* transport);

private:
    std::uint64_t       _tickCount{0};
    io::IODeviceManager _deviceManager;
    io::RoutingManager  _routing;
    io::IOService       _ioService;
    fs::StorageManager  _storageManager;
};

} // namespace fujinet::core
