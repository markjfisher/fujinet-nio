#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/core/logging.h"

namespace fujinet::core {

using fujinet::io::DeviceID;
using fujinet::io::NetworkDevice;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static const char* TAG = "core";

void register_network_device(FujinetCore& core)
{
    // IMPORTANT: only register ONE network device for now.
    // We can discuss later how to scale out without allocating/registering
    // multiple device instances up front.
    auto dev = std::make_unique<NetworkDevice>();
    DeviceID id = to_device_id(WireDeviceId::NetworkService); // 0xFD

    bool ok = core.deviceManager().registerDevice(id, std::move(dev));
    if (!ok) {
        FN_LOGE(TAG, "Failed to register NetworkDevice on DeviceID %u", static_cast<unsigned>(id));
    } else {
        FN_LOGI(TAG, "Registered NetworkDevice on DeviceID %u", static_cast<unsigned>(id));
    }
}

} // namespace fujinet::core


