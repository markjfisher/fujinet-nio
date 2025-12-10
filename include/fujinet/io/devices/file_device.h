#pragma once

#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/fs/storage_manager.h"

namespace fujinet::io {

class FileDevice : public VirtualDevice {
public:
    explicit FileDevice(fs::StorageManager& storage);

    IOResponse handle(const IORequest& request) override;
    void poll() override {} // no background work (for now)

private:
    fs::StorageManager& _storage;

    IOResponse handle_list_directory(const IORequest& request);
};

} // namespace fujinet::io
