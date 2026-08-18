#include "fujinet/platform/posix/wifi_link.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/types.h>
#endif

namespace fujinet::platform::posix {
namespace {

std::string env_or(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

std::string ipv4_text(const sockaddr* address)
{
#if defined(__linux__) || defined(__APPLE__)
    if (!address || address->sa_family != AF_INET) return {};
    char text[INET_ADDRSTRLEN]{};
    const auto* in = reinterpret_cast<const sockaddr_in*>(address);
    return inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) ? text : std::string{};
#else
    (void)address;
    return {};
#endif
}

} // namespace

PosixWifiLink::PosixWifiLink(WifiBackendMode mode, std::string interfaceName)
    : _mode(mode), _interface(std::move(interfaceName))
{}

WifiBackendMode PosixWifiLink::mode_from_environment()
{
    const auto mode = env_or("FN_POSIX_WIFI_BACKEND", "simulated");
    if (mode == "host") return WifiBackendMode::Host;
    if (mode == "unavailable" || mode == "none") return WifiBackendMode::Unavailable;
    return WifiBackendMode::Simulated;
}

net::LinkState PosixWifiLink::state() const
{
    return _state;
}

void PosixWifiLink::connect(std::string ssid, std::string pass)
{
    _ssid = std::move(ssid);
    _password = std::move(pass);
    if (_mode == WifiBackendMode::Simulated) {
        _state = net::LinkState::Connected;
        _ip = "192.0.2.2";
        _subnet = "255.255.255.0";
        _gateway = "192.0.2.1";
        _dns = "192.0.2.1";
    } else if (_mode == WifiBackendMode::Host) {
        // POSIX host Wi-Fi is managed by the operating system, not FujiNet.
        refresh_host_state();
    } else {
        _state = net::LinkState::Failed;
    }
}

void PosixWifiLink::set_bssid(std::string bssid)
{
    _bssid_text = std::move(bssid);
}

void PosixWifiLink::disconnect()
{
    if (_mode == WifiBackendMode::Simulated) {
        _state = net::LinkState::Disconnected;
        _ip.clear();
        _subnet.clear();
        _gateway.clear();
        _dns.clear();
    } else if (_mode == WifiBackendMode::Host) {
        // Deliberately do not disrupt the host's network connection.
        refresh_host_state();
    } else {
        _state = net::LinkState::Disconnected;
    }
}

void PosixWifiLink::poll()
{
    if (_mode == WifiBackendMode::Host) refresh_host_state();
}

std::string PosixWifiLink::ip_address() const { return _ip; }
net::WifiBssid PosixWifiLink::current_bssid() const { return {}; }
std::int8_t PosixWifiLink::rssi() const { return 0; }
std::string PosixWifiLink::subnet_mask() const { return _subnet; }
std::string PosixWifiLink::gateway() const { return _gateway; }
std::string PosixWifiLink::dns_server() const { return _dns; }

net::WifiLinkCapabilities PosixWifiLink::capabilities() const
{
    switch (_mode) {
    case WifiBackendMode::Host:
        return {static_cast<std::uint16_t>(net::WifiCapabilityConfig |
                                            net::WifiCapabilityStatus |
                                            net::WifiCapabilityHostManaged),
                net::WifiBackendKind::PosixHost};
    case WifiBackendMode::Simulated:
        return {static_cast<std::uint16_t>(net::WifiCapabilityConfig |
                                            net::WifiCapabilityStatus |
                                            net::WifiCapabilityConnect |
                                            net::WifiCapabilityDisconnect |
                                            net::WifiCapabilityScan |
                                            net::WifiCapabilityBssidSelect |
                                            net::WifiCapabilitySimulated),
                net::WifiBackendKind::PosixSimulated};
    case WifiBackendMode::Unavailable:
    default:
        return {static_cast<std::uint16_t>(net::WifiCapabilityConfig |
                                            net::WifiCapabilityStatus),
                net::WifiBackendKind::Unavailable};
    }
}

bool PosixWifiLink::supports_scan() const
{
    return _mode == WifiBackendMode::Simulated;
}

net::WifiScanResult PosixWifiLink::scan_wifi()
{
    net::WifiScanResult result;
    if (!supports_scan()) return result;

    const std::uint8_t bssids[][6] = {
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x01},
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x02},
        {0x02, 0x00, 0x00, 0x00, 0x00, 0x03},
    };
    const char* names[] = {"FujiNet-Sim", "FujiNet-Test", "FujiNet-Guest"};
    const std::int8_t rssis[] = {-42, -58, -71};
    const std::uint8_t channels[] = {1, 6, 11};
    for (std::size_t i = 0; i < 3; ++i) {
        net::WifiScanRecord record;
        record.ssid = names[i];
        std::memcpy(record.bssid.bytes, bssids[i], 6);
        record.bssid.valid = true;
        record.rssi = rssis[i];
        record.channel = channels[i];
        record.auth = i == 2 ? 0 : 1;
        result.records.push_back(std::move(record));
    }
    result.success = true;
    return result;
}

void PosixWifiLink::refresh_host_state()
{
#if defined(__linux__) || defined(__APPLE__)
    struct ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) {
        _state = net::LinkState::Failed;
        return;
    }
    _ip.clear();
    for (auto* item = addresses; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) continue;
        if (!_interface.empty() && _interface != item->ifa_name) continue;
        const auto value = ipv4_text(item->ifa_addr);
        if (value.empty() || value == "127.0.0.1") continue;
        _ip = value;
        _subnet = ipv4_text(item->ifa_netmask);
        break;
    }
    freeifaddrs(addresses);
    _state = _ip.empty() ? net::LinkState::Disconnected : net::LinkState::Connected;
#else
    _state = net::LinkState::Disconnected;
#endif
}

} // namespace fujinet::platform::posix
