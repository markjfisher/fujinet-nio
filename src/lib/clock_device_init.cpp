#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/clock_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/core/logging.h"

namespace fujinet::core {

using fujinet::io::ClockDevice;
using fujinet::io::DeviceID;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static const char* TAG = "core";

void register_clock_device(FujinetCore& core)
{
    auto dev = std::make_unique<ClockDevice>();
    DeviceID id = to_device_id(WireDeviceId::Clock);

    bool ok = core.deviceManager().registerDevice(id, std::move(dev));
    if (!ok) {
        FN_LOGE(TAG, "Failed to register ClockDevice on DeviceID %u", static_cast<unsigned>(id));
    } else {
        FN_LOGI(TAG, "Registered ClockDevice on DeviceID %u", static_cast<unsigned>(id));
    }
}

} // namespace fujinet::core
