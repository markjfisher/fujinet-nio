#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/file_device.h"
#include "fujinet/io/protocol/fuji_device_ids.h"
#include "fujinet/core/logging.h"

namespace fujinet::core {

using fujinet::io::FileDevice;
using fujinet::io::DeviceID;
using fujinet::io::protocol::FujiDeviceId;

static const char* TAG = "core";

void register_file_device(FujinetCore& core)
{
    auto dev = std::make_unique<FileDevice>(core.storageManager());
    constexpr DeviceID fileDeviceId =
        static_cast<DeviceID>(FujiDeviceId::FileService);

    bool ok = core.deviceManager().registerDevice(fileDeviceId, std::move(dev));
    if (!ok) {
        FN_LOGE(TAG, "Failed to register FileDevice on DeviceID %u",
                static_cast<unsigned>(fileDeviceId));
    } else {
        FN_LOGI(TAG, "Registered FileDevice on DeviceID %u",
                static_cast<unsigned>(fileDeviceId));
    }
}

} // namespace fujinet::core
