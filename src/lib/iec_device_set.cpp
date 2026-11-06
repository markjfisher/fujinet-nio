#include "fujinet/io/iec_device_set.h"

namespace fujinet::io {

IecDeviceSet::IecDeviceSet(disk::DiskService& disks, IRequestHandler& requests)
    : _nio(requests)
{
    for (std::size_t slot = 0; slot < disks.slot_count(); ++slot) {
        const bool alwaysPresent = slot == 0;
        _drives.push_back(std::make_unique<IecDriveFrontend>(disks, slot, alwaysPresent));
        _bus.attach(static_cast<std::uint8_t>(IecDriveFrontend::first_device + slot), *_drives.back());
    }
    _bus.attach(IecNioFrontend::default_device, _nio);
}

} // namespace fujinet::io
