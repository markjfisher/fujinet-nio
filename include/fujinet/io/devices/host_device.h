#pragma once

#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/virtual_device.h"

namespace fujinet::io {

class HostDevice : public VirtualDevice {
public:
    explicit HostDevice(fs::StorageManager& storage);

    IOResponse handle(const IORequest& request) override;

private:
    fs::StorageManager& _storage;
};

} // namespace fujinet::io
