#pragma once

#include <functional>
#include <utility>

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/net/network_link.h"

namespace fujinet::io {

// Shared Wi-Fi state boundary used by the wire service and diagnostics. It
// owns no link; the platform supplies the provider and controls its lifetime.
class WifiController {
public:
    using LinkProvider = std::function<net::INetworkLink*()>;

    WifiController(config::FujiConfig& config,
                   config::FujiConfigStore* store,
                   LinkProvider linkProvider)
        : _config(config), _store(store), _linkProvider(std::move(linkProvider))
    {}

    const config::WifiConfig& config() const { return _config.wifi; }
    config::WifiConfig& config_mut() { return _config.wifi; }
    net::INetworkLink* link() const { return _linkProvider ? _linkProvider() : nullptr; }

    StatusCode save() const;
    StatusCode update(const config::WifiConfig& next, bool persist, bool reconnect);
    StatusCode reconnect();
    StatusCode disconnect();
    net::WifiScanResult scan() const;

private:
    config::FujiConfig& _config;
    config::FujiConfigStore* _store;
    LinkProvider _linkProvider;
};

} // namespace fujinet::io
