#include "fujinet/io/devices/host_device.h"

#include "fujinet/core/logging.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/byte_codec.h"
#include "fujinet/io/devices/host_commands.h"
#include "fujinet/io/protocol/wire_device_ids.h"

namespace fujinet::io {

using bytecodec::Reader;

static const char* TAG = "host";

static StatusCode map_host_status(HostStatus s) noexcept
{
    switch (s) {
        case HostStatus::OK:            return StatusCode::Ok;
        case HostStatus::InvalidSlot:   return StatusCode::InvalidRequest;
        case HostStatus::InvalidParam:  return StatusCode::InvalidRequest;
        case HostStatus::NotConfigured: return StatusCode::InvalidRequest;
        case HostStatus::UnknownCommand:return StatusCode::Unsupported;
    }
    return StatusCode::InternalError;
}

static std::uint8_t host_type_to_wire(config::HostType t) noexcept
{
    switch (t) {
        case config::HostType::Sd:   return static_cast<std::uint8_t>(HostType::SD);
        case config::HostType::Tnfs: return static_cast<std::uint8_t>(HostType::TNFS);
        default:                     return static_cast<std::uint8_t>(HostType::Disabled);
    }
}

static config::HostType wire_to_host_type(std::uint8_t t) noexcept
{
    switch (t) {
        case static_cast<std::uint8_t>(HostType::SD):   return config::HostType::Sd;
        case static_cast<std::uint8_t>(HostType::TNFS): return config::HostType::Tnfs;
        default:                                        return config::HostType::Unknown;
    }
}

HostDevice::HostDevice(config::FujiConfig& config)
    : _config(config)
{
}

IOResponse HostDevice::handle(const IORequest& request)
{
    const auto cmd = static_cast<HostCommand>(request.command);

    Reader r(request.payload.data(), request.payload.size());
    std::uint8_t ver = 0;
    if (!r.read_u8(ver) || ver != HOSTPROTO_VERSION) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    switch (cmd) {
        case HostCommand::GetHosts:
            return handle_get_hosts(request);
        case HostCommand::SetHost:
            return handle_set_host(request);
        case HostCommand::GetHost:
            return handle_get_host(request);
        default:
            return handle_unknown(request);
    }
}

IOResponse HostDevice::handle_get_hosts(const IORequest& request)
{
    // Response: [version:1][host_count:1][entries...]
    // Each entry: [type:1][name:32][address:64] = 97 bytes
    std::vector<std::uint8_t> payload;
    payload.reserve(2 + MAX_HOSTS * (1 + MAX_HOST_NAME_LEN + MAX_HOST_ADDR_LEN));

    bytecodec::write_u8(payload, HOSTPROTO_VERSION);

    // Count enabled hosts
    std::uint8_t host_count = 0;
    for (const auto& host : _config.hosts) {
        if (host.enabled) host_count++;
    }
    bytecodec::write_u8(payload, host_count);

    // Write each enabled host
    for (const auto& host : _config.hosts) {
        if (!host.enabled) continue;

        // Type
        bytecodec::write_u8(payload, host_type_to_wire(host.type));

        // Name (fixed 32 bytes, null-padded)
        std::string name = host.name;
        if (name.size() > MAX_HOST_NAME_LEN) name.resize(MAX_HOST_NAME_LEN);
        bytecodec::write_bytes(payload, name.data(), name.size());
        for (std::size_t i = name.size(); i < MAX_HOST_NAME_LEN; ++i) {
            bytecodec::write_u8(payload, 0);
        }

        // Address (fixed 64 bytes, null-padded)
        std::string addr = host.address;
        if (addr.size() > MAX_HOST_ADDR_LEN) addr.resize(MAX_HOST_ADDR_LEN);
        bytecodec::write_bytes(payload, addr.data(), addr.size());
        for (std::size_t i = addr.size(); i < MAX_HOST_ADDR_LEN; ++i) {
            bytecodec::write_u8(payload, 0);
        }
    }

    IOResponse resp = make_base_response(request, StatusCode::Ok);
    resp.payload = std::move(payload);
    return resp;
}

IOResponse HostDevice::handle_set_host(const IORequest& request)
{
    // Request: [version:1][slot:1][type:1][name:32][address:64]
    Reader r(request.payload.data(), request.payload.size());

    std::uint8_t ver = 0, slot = 0, typeRaw = 0;
    if (!r.read_u8(ver)) return make_base_response(request, StatusCode::InvalidRequest);
    if (!r.read_u8(slot)) return make_base_response(request, StatusCode::InvalidRequest);
    if (!r.read_u8(typeRaw)) return make_base_response(request, StatusCode::InvalidRequest);

    if (slot >= MAX_HOSTS) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    // Read fixed-length name
    std::string name;
    for (std::size_t i = 0; i < MAX_HOST_NAME_LEN; ++i) {
        std::uint8_t c = 0;
        if (!r.read_u8(c)) return make_base_response(request, StatusCode::InvalidRequest);
        if (c != 0) name.push_back(static_cast<char>(c));
    }

    // Read fixed-length address
    std::string addr;
    for (std::size_t i = 0; i < MAX_HOST_ADDR_LEN; ++i) {
        std::uint8_t c = 0;
        if (!r.read_u8(c)) return make_base_response(request, StatusCode::InvalidRequest);
        if (c != 0) addr.push_back(static_cast<char>(c));
    }

    // Update or add host config
    // Find existing host with matching slot id or add new
    config::HostConfig* hostPtr = nullptr;
    for (auto& h : _config.hosts) {
        if (h.id == static_cast<int>(slot)) {
            hostPtr = &h;
            break;
        }
    }

    if (!hostPtr) {
        // Add new host
        config::HostConfig newHost;
        newHost.id = static_cast<int>(slot);
        _config.hosts.push_back(newHost);
        hostPtr = &_config.hosts.back();
    }

    if (typeRaw == static_cast<std::uint8_t>(HostType::Disabled)) {
        hostPtr->enabled = false;
    } else {
        hostPtr->type = wire_to_host_type(typeRaw);
        hostPtr->name = name;
        hostPtr->address = addr;
        hostPtr->enabled = true;
    }

    // Response: [version:1][status:1]
    std::vector<std::uint8_t> payload;
    bytecodec::write_u8(payload, HOSTPROTO_VERSION);
    bytecodec::write_u8(payload, static_cast<std::uint8_t>(HostStatus::OK));

    IOResponse resp = make_base_response(request, StatusCode::Ok);
    resp.payload = std::move(payload);
    return resp;
}

IOResponse HostDevice::handle_get_host(const IORequest& request)
{
    // Request: [version:1][slot:1]
    Reader r(request.payload.data(), request.payload.size());

    std::uint8_t ver = 0, slot = 0;
    if (!r.read_u8(ver)) return make_base_response(request, StatusCode::InvalidRequest);
    if (!r.read_u8(slot)) return make_base_response(request, StatusCode::InvalidRequest);

    if (slot >= MAX_HOSTS) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    // Find host by id
    const config::HostConfig* hostPtr = nullptr;
    for (const auto& h : _config.hosts) {
        if (h.id == static_cast<int>(slot) && h.enabled) {
            hostPtr = &h;
            break;
        }
    }

    if (!hostPtr) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    // Response: [version:1][type:1][name:32][address:64]
    std::vector<std::uint8_t> payload;
    payload.reserve(1 + 1 + MAX_HOST_NAME_LEN + MAX_HOST_ADDR_LEN);

    bytecodec::write_u8(payload, HOSTPROTO_VERSION);
    bytecodec::write_u8(payload, host_type_to_wire(hostPtr->type));

    // Name (fixed 32 bytes, null-padded)
    std::string name = hostPtr->name;
    if (name.size() > MAX_HOST_NAME_LEN) name.resize(MAX_HOST_NAME_LEN);
    bytecodec::write_bytes(payload, name.data(), name.size());
    for (std::size_t i = name.size(); i < MAX_HOST_NAME_LEN; ++i) {
        bytecodec::write_u8(payload, 0);
    }

    // Address (fixed 64 bytes, null-padded)
    std::string addr = hostPtr->address;
    if (addr.size() > MAX_HOST_ADDR_LEN) addr.resize(MAX_HOST_ADDR_LEN);
    bytecodec::write_bytes(payload, addr.data(), addr.size());
    for (std::size_t i = addr.size(); i < MAX_HOST_ADDR_LEN; ++i) {
        bytecodec::write_u8(payload, 0);
    }

    IOResponse resp = make_base_response(request, StatusCode::Ok);
    resp.payload = std::move(payload);
    return resp;
}

IOResponse HostDevice::handle_unknown(const IORequest& request)
{
    FN_LOGW(TAG, "unknown command 0x%02X", request.command);
    return make_base_response(request, StatusCode::Unsupported);
}

} // namespace fujinet::io
