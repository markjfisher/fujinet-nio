#include "fujinet/io/devices/wifi_controller.h"

#include <cstddef>
#include <string_view>

namespace fujinet::io {
namespace {

bool valid_bssid(std::string_view text)
{
    if (text.empty()) return true;
    if (text.size() != 17) return false;
    auto hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    };
    for (std::size_t i = 0; i < 6; ++i) {
        if ((i && text[i * 3 - 1] != ':') || !hex(text[i * 3]) || !hex(text[i * 3 + 1]))
            return false;
    }
    return true;
}

} // namespace

StatusCode WifiController::save() const
{
    if (!_store) return StatusCode::NotReady;
    _store->save(_config);
    return StatusCode::Ok;
}

StatusCode WifiController::update(const config::WifiConfig& next,
                                  bool persist,
                                  bool reconnect)
{
    if (!valid_bssid(next.bssid)) return StatusCode::InvalidRequest;
    auto* currentLink = reconnect ? link() : nullptr;
    if (persist && !_store) return StatusCode::NotReady;
    if (reconnect) {
        if (!currentLink) return StatusCode::NotReady;
        const auto capabilities = currentLink->capabilities().flags;
        const auto required = next.enabled ? net::WifiCapabilityConnect
                                           : net::WifiCapabilityDisconnect;
        if (!(capabilities & required)) return StatusCode::Unsupported;
        if (!next.bssid.empty() && !(capabilities & net::WifiCapabilityBssidSelect))
            return StatusCode::Unsupported;
    }

    _config.wifi = next;
    if (persist) _store->save(_config);
    if (reconnect) {
        currentLink->set_bssid(_config.wifi.bssid);
        if (_config.wifi.enabled)
            currentLink->connect(_config.wifi.ssid, _config.wifi.passphrase);
        else
            currentLink->disconnect();
    }
    return StatusCode::Ok;
}

StatusCode WifiController::reconnect()
{
    if (_config.wifi.ssid.empty()) return StatusCode::InvalidRequest;
    auto* currentLink = link();
    if (!currentLink) return StatusCode::NotReady;
    const auto capabilities = currentLink->capabilities().flags;
    if (!(capabilities & net::WifiCapabilityConnect)) return StatusCode::Unsupported;
    if (!(_config.wifi.bssid.empty() || (capabilities & net::WifiCapabilityBssidSelect)))
        return StatusCode::Unsupported;
    _config.wifi.enabled = true;
    currentLink->set_bssid(_config.wifi.bssid);
    currentLink->connect(_config.wifi.ssid, _config.wifi.passphrase);
    return StatusCode::Ok;
}

StatusCode WifiController::disconnect()
{
    auto* currentLink = link();
    if (!currentLink) return StatusCode::NotReady;
    if (!(currentLink->capabilities().flags & net::WifiCapabilityDisconnect))
        return StatusCode::Unsupported;
    currentLink->disconnect();
    return StatusCode::Ok;
}

net::WifiScanResult WifiController::scan() const
{
    auto* currentLink = link();
    if (!currentLink || !(currentLink->capabilities().flags & net::WifiCapabilityScan))
        return {};
    return currentLink->scan_wifi();
}

} // namespace fujinet::io
