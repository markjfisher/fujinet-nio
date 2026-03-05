#pragma once

#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/virtual_device.h"

namespace fujinet::io {

// DiskDevice: generic disk-image mount + sector read/write service (v1).
// Wire device ID: WireDeviceId::DiskService (0xFC).
//
// This is NOT a machine-specific disk protocol (SIO/DFS/etc).
// Machine-specific disk devices should reuse fujinet::disk::DiskService.
class DiskDevice : public VirtualDevice {
public:
    explicit DiskDevice(fs::StorageManager& storage, disk::ImageRegistry registry);
    explicit DiskDevice(fs::StorageManager& storage);

    IOResponse handle(const IORequest& request) override;
    void poll() override {}

    // Access to the underlying DiskService for config mount application
    disk::DiskService& disk_service() { return _svc; }
    const disk::DiskService& disk_service() const { return _svc; }

private:
    // Allow out-of-band diagnostics (console) without polluting the on-wire API surface.
    friend struct DiskDeviceDiagnosticsAccessor;

    static constexpr std::uint8_t DISKPROTO_VERSION = 1;

    fs::StorageManager& _storage;
    disk::DiskService _svc;
};

} // namespace fujinet::io


