#pragma once

#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/virtual_device.h"

namespace fujinet::io {

// Management service for current host state and history. This endpoint is
// addressable over the same IORequest path as devices, but it manages FujiNet
// state rather than representing a virtual hardware device.
class HostService : public VirtualService {
public:
    explicit HostService(fs::StorageManager& storage);

    IOResponse handle(const IORequest& request) override;

private:
    fs::StorageManager& _storage;
};

} // namespace fujinet::io
