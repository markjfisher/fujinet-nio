#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/disk_registry.h"

namespace fujinet::core {

using fujinet::io::DeviceID;
using fujinet::io::DiskDevice;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static const char* TAG = "core";

void register_disk_device(FujinetCore& core)
{
    auto reg = fujinet::platform::make_default_disk_image_registry();
    auto dev = std::make_unique<DiskDevice>(core.storageManager(), std::move(reg));
    DeviceID id = to_device_id(WireDeviceId::DiskService); // 0xFC

    bool ok = core.deviceManager().registerDevice(id, std::move(dev));
    if (!ok) {
        FN_LOGE(TAG, "Failed to register DiskDevice on DeviceID %u", static_cast<unsigned>(id));
    } else {
        FN_ELOG("Registered DiskDevice on DeviceID %u", static_cast<unsigned>(id));
    }
}

} // namespace fujinet::core


