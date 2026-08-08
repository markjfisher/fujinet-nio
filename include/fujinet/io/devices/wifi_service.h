#pragma once

#include <functional>
#include <vector>

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/devices/wifi_controller.h"
#include "fujinet/net/network_link.h"

namespace fujinet::io {

class WifiService final : public VirtualService {
public:
    using LinkProvider = std::function<net::INetworkLink*()>;

    WifiService(config::FujiConfig& config,
                config::FujiConfigStore* store,
                LinkProvider linkProvider);

    IOResponse handle(const IORequest& request) override;

private:
    WifiController _controller;
    std::vector<net::WifiScanRecord> _scanCache;
    bool _scanCacheValid{false};
};

} // namespace fujinet::io
