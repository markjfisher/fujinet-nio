#include "fujinet/core/wifi_service_init.h"

#include "fujinet/io/devices/wifi_service.h"
#include "fujinet/io/protocol/wire_device_ids.h"

namespace fujinet::core {

void register_wifi_service(FujinetCore& core,
                           config::FujiConfig& config,
                           config::FujiConfigStore* store,
                           std::function<net::INetworkLink*()> linkProvider)
{
    core.deviceManager().registerDevice(
        io::protocol::to_device_id(io::protocol::WireDeviceId::WifiService),
        std::make_unique<io::WifiService>(config, store, std::move(linkProvider)));
}

} // namespace fujinet::core
