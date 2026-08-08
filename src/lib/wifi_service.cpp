#include "fujinet/io/devices/wifi_service.h"

#include "fujinet/io/devices/wifi_service_commands.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>

namespace fujinet::io {
namespace {

constexpr std::uint8_t VERSION = 1;
constexpr std::size_t MAX_SSID = 32;
constexpr std::size_t MAX_BSSID_TEXT = 17;
constexpr std::size_t MAX_PASSWORD = 64;
constexpr std::size_t MAX_SCAN_RECORDS = 32;

bool u8(const std::vector<std::uint8_t>& p, std::size_t& at, std::uint8_t& v)
{
    if (at >= p.size()) return false;
    v = p[at++];
    return true;
}

bool u16(const std::vector<std::uint8_t>& p, std::size_t& at, std::uint16_t& v)
{
    if (at + 2 > p.size()) return false;
    v = static_cast<std::uint16_t>(p[at]) |
        static_cast<std::uint16_t>(p[at + 1]) << 8;
    at += 2;
    return true;
}

bool string8(const std::vector<std::uint8_t>& p, std::size_t& at,
             std::size_t max, std::string& out)
{
    std::uint8_t len = 0;
    if (!u8(p, at, len) || len > max || at + len > p.size()) return false;
    out.assign(reinterpret_cast<const char*>(p.data() + at), len);
    at += len;
    return true;
}

void put16(std::vector<std::uint8_t>& p, std::uint16_t v)
{
    p.push_back(static_cast<std::uint8_t>(v));
    p.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put_string8(std::vector<std::uint8_t>& p, const std::string& s)
{
    p.push_back(static_cast<std::uint8_t>(s.size()));
    p.insert(p.end(), s.begin(), s.end());
}

} // namespace

WifiService::WifiService(config::FujiConfig& config,
                         config::FujiConfigStore* store,
                         LinkProvider linkProvider)
    : _controller(config, store, std::move(linkProvider))
{}

IOResponse WifiService::handle(const IORequest& request)
{
    if (request.payload.empty() || request.payload[0] != VERSION)
        return make_base_response(request, StatusCode::InvalidRequest);

    auto response = make_success_response(request);
    const auto command = protocol::to_wifi_command(request.command);
    if (command == protocol::WifiCommand::GetConfig) {
        if (request.payload.size() != 1 || _controller.config().ssid.size() > MAX_SSID ||
            _controller.config().bssid.size() > MAX_BSSID_TEXT)
            return make_base_response(request, StatusCode::InvalidRequest);
        response.payload = {VERSION, static_cast<std::uint8_t>(_controller.config().enabled),
                            static_cast<std::uint8_t>(!_controller.config().passphrase.empty())};
        put_string8(response.payload, _controller.config().ssid);
        put_string8(response.payload, _controller.config().bssid);
        return response;
    }

    if (command == protocol::WifiCommand::GetStatus) {
        if (request.payload.size() != 1) return make_base_response(request, StatusCode::InvalidRequest);
        auto* link = _controller.link();
        if (!link) return make_base_response(request, StatusCode::NotReady);
        const auto bssid = link->current_bssid();
        const auto caps = link->capabilities();
        response.payload = {VERSION, static_cast<std::uint8_t>(link->state()),
                            static_cast<std::uint8_t>(_controller.config().enabled),
                            static_cast<std::uint8_t>(bssid.valid ? 1 : 0),
                            static_cast<std::uint8_t>(caps.flags & net::WifiCapabilityScan ? 1 : 0),
                            static_cast<std::uint8_t>(link->rssi())};
        response.payload.insert(response.payload.end(), bssid.bytes, bssid.bytes + 6);
        put_string8(response.payload, link->ip_address());
        put_string8(response.payload, link->subnet_mask());
        put_string8(response.payload, link->gateway());
        put_string8(response.payload, link->dns_server());
        // Extensions follow the version-1 fields so older clients can still parse status.
        put16(response.payload, caps.flags);
        response.payload.push_back(static_cast<std::uint8_t>(caps.backend));
        return response;
    }

    if (command == protocol::WifiCommand::SetConfig) {
        if (request.payload.size() < 2) return make_base_response(request, StatusCode::InvalidRequest);
        std::size_t at = 1;
        std::uint8_t flags = request.payload[at++];
        if (flags & static_cast<std::uint8_t>(~0x3Fu))
            return make_base_response(request, StatusCode::InvalidRequest);
        config::WifiConfig next = _controller.config();
        if (flags & 0x01) {
            std::uint8_t enabled = 0;
            if (!u8(request.payload, at, enabled)) return make_base_response(request, StatusCode::InvalidRequest);
            next.enabled = enabled != 0;
        }
        if (flags & 0x02 && !string8(request.payload, at, MAX_SSID, next.ssid))
            return make_base_response(request, StatusCode::InvalidRequest);
        if (flags & 0x04 && !string8(request.payload, at, MAX_BSSID_TEXT, next.bssid))
            return make_base_response(request, StatusCode::InvalidRequest);
        if (flags & 0x08 && !string8(request.payload, at, MAX_PASSWORD, next.passphrase))
            return make_base_response(request, StatusCode::InvalidRequest);
        if (next.bssid.size() > MAX_BSSID_TEXT || at != request.payload.size())
            return make_base_response(request, StatusCode::InvalidRequest);
        const auto updateStatus = _controller.update(next, (flags & 0x10) != 0, (flags & 0x20) != 0);
        if (updateStatus != StatusCode::Ok) return make_base_response(request, updateStatus);
        response.payload = {VERSION};
        return response;
    }

    if (command == protocol::WifiCommand::Scan) {
        std::size_t at = 1;
        std::uint16_t offset = 0;
        std::uint8_t limit = 0;
        if (!u16(request.payload, at, offset) || !u8(request.payload, at, limit) ||
            at != request.payload.size() || limit == 0 || limit > MAX_SCAN_RECORDS)
            return make_base_response(request, StatusCode::InvalidRequest);
        auto* link = _controller.link();
        if (!link || !(link->capabilities().flags & net::WifiCapabilityScan))
            return make_base_response(request, StatusCode::Unsupported);
        if (offset == 0 || !_scanCacheValid) {
            const auto scan = _controller.scan();
            if (!scan.success) return make_base_response(request, StatusCode::IOError);
            _scanCache = scan.records;
            _scanCacheValid = true;
        }
        const std::size_t start = std::min<std::size_t>(offset, _scanCache.size());
        const std::size_t count = std::min<std::size_t>(limit, _scanCache.size() - start);
        response.payload = {VERSION, static_cast<std::uint8_t>(start + count < _scanCache.size()),
                            static_cast<std::uint8_t>(count)};
        for (std::size_t i = 0; i < count; ++i) {
            const auto& record = _scanCache[start + i];
            if (record.ssid.size() > MAX_SSID) return make_base_response(request, StatusCode::InternalError);
            put_string8(response.payload, record.ssid);
            response.payload.insert(response.payload.end(), record.bssid.bytes, record.bssid.bytes + 6);
            response.payload.push_back(static_cast<std::uint8_t>(record.rssi));
            response.payload.push_back(record.channel);
            response.payload.push_back(record.auth);
        }
        return response;
    }

    return make_base_response(request, StatusCode::Unsupported);
}

} // namespace fujinet::io
