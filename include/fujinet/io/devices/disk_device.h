#pragma once

#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/virtual_device.h"

#include <string>
#include <vector>
#include <optional>

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

    void configure_boot_mount(std::string configUri, bool readOnly);
    std::vector<std::size_t> restore_runtime_mounts();

    // Access to the underlying DiskService for config mount application
    disk::DiskService& disk_service() { return _svc; }
    const disk::DiskService& disk_service() const { return _svc; }

private:
    // Allow out-of-band diagnostics (console) without polluting the on-wire API surface.
    friend struct DiskDeviceDiagnosticsAccessor;

    static constexpr std::uint8_t DISKPROTO_VERSION = 1;

    struct RuntimeMountState {
        std::string uri;
        std::string mode;
        std::uint16_t sectorSizeHint{0};
    };

    bool save_runtime_mounts();
    bool load_runtime_mounts();
    bool clear_runtime_mounts();
    void set_runtime_mount(std::size_t slotIndex, RuntimeMountState state);
    void clear_runtime_mount(std::size_t slotIndex);

    fs::StorageManager& _storage;
    disk::DiskService _svc;
    std::string _bootConfigUri;
    bool _bootReadOnly{true};
    std::vector<std::optional<RuntimeMountState>> _runtimeMounts;
};

} // namespace fujinet::io
