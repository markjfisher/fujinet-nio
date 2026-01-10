#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/logging.h"
#include "fujinet/io/devices/modem_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/platform/tcp_socket_ops.h"

namespace fujinet::core {

using fujinet::io::DeviceID;
using fujinet::io::ModemDevice;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static const char* TAG = "core";

void register_modem_device(FujinetCore& core)
{
    auto& ops = fujinet::platform::default_tcp_socket_ops();
    auto dev = std::make_unique<ModemDevice>(ops);
    DeviceID id = to_device_id(WireDeviceId::ModemService); // 0xFB

    bool ok = core.deviceManager().registerDevice(id, std::move(dev));
    if (!ok) {
        FN_LOGE(TAG, "Failed to register ModemDevice on DeviceID %u", static_cast<unsigned>(id));
    } else {
        FN_ELOG("Registered ModemDevice on DeviceID %u", static_cast<unsigned>(id));
    }
}

} // namespace fujinet::core


