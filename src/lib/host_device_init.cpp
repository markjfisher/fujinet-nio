#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/io/devices/host_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/core/logging.h"

namespace fujinet::core {

using fujinet::io::DeviceID;
using fujinet::io::HostDevice;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static const char* TAG = "core";

void register_host_device(FujinetCore& core, config::FujiConfig& config)
{
    auto dev = std::make_unique<HostDevice>(config);
    DeviceID id = to_device_id(WireDeviceId::HostService); // 0xF0

    bool ok = core.deviceManager().registerDevice(id, std::move(dev));
    if (!ok) {
        FN_LOGE(TAG, "Failed to register HostDevice on DeviceID %u", static_cast<unsigned>(id));
    } else {
        FN_ELOG("Registered HostDevice on DeviceID %u", static_cast<unsigned>(id));
    }
}

} // namespace fujinet::core
