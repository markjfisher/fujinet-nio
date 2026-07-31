#include "fujinet/core/device_init.h"

#include "fujinet/core/logging.h"
#include "fujinet/io/devices/app_store.h"
#include "fujinet/io/devices/app_store_service.h"
#include "fujinet/io/devices/slot_catalog_service.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <memory>

namespace fujinet::core {

namespace {

constexpr const char* TAG = "core";

bool register_service(
    FujinetCore& core,
    io::protocol::WireDeviceId wireId,
    std::unique_ptr<io::VirtualService> service,
    const char* name)
{
    const auto id = io::protocol::to_device_id(wireId);
    if (!core.deviceManager().registerDevice(id, std::move(service))) {
        FN_LOGE(TAG, "Failed to register %s on DeviceID %u",
                name, static_cast<unsigned>(id));
        return false;
    }
    FN_ELOG("Registered %s on DeviceID %u",
            name, static_cast<unsigned>(id));
    return true;
}

} // namespace

void register_application_state_services(FujinetCore& core)
{
    auto store = std::make_shared<io::AppStore>(core.storageManager());
    register_service(
        core,
        io::protocol::WireDeviceId::AppStoreService,
        std::make_unique<io::AppStoreService>(store),
        "AppStoreService");
    register_service(
        core,
        io::protocol::WireDeviceId::SlotCatalogService,
        std::make_unique<io::SlotCatalogService>(
            core.storageManager(), std::move(store)),
        "SlotCatalogService");
}

} // namespace fujinet::core
