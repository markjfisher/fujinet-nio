#pragma once

#include "fujinet/disk/disk_service.h"
#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/iec_bus.h"
#include "fujinet/io/iec_drive_frontend.h"
#include "fujinet/io/iec_nio_frontend.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace fujinet::io {

// The devices a FujiNet presents on a Commodore bus: drives 8-15, one per DiskService
// slot, and the NIO packet channel. An ingress drives bus() however the bus reaches it.
class IecDeviceSet {
public:
    IecDeviceSet(disk::DiskService& disks, IRequestHandler& requests);

    IecBus& bus() noexcept { return _bus; }

private:
    IecBus _bus;
    IecNioFrontend _nio;
    std::vector<std::unique_ptr<IecDriveFrontend>> _drives;
};

} // namespace fujinet::io
