#include "fujinet/io/devices/host_device.h"

#include "fujinet/io/devices/host_commands.h"
#include "fujinet/io/host_state.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace fujinet::io {

namespace {

constexpr std::uint8_t HOSTPROTO_VERSION = 1;
constexpr std::uint8_t HOST_LIST_MORE = 0x01;

bool read_u16le(const std::vector<std::uint8_t>& data, std::size_t off, std::uint16_t& out)
{
    if (off + 2 > data.size()) return false;
    out = static_cast<std::uint16_t>(data[off]) |
          static_cast<std::uint16_t>(data[off + 1] << 8);
    return true;
}

void write_u8(std::vector<std::uint8_t>& out, std::uint8_t value)
{
    out.push_back(value);
}

void write_u16le(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

bool valid_version(const IORequest& request)
{
    return !request.payload.empty() && request.payload[0] == HOSTPROTO_VERSION;
}

} // namespace

HostDevice::HostDevice(fs::StorageManager& storage)
    : _storage(storage)
{}

IOResponse HostDevice::handle(const IORequest& request)
{
    using protocol::HostCommand;
    auto resp = make_success_response(request);
    HostState hostState(_storage);

    if (!valid_version(request)) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    switch (protocol::to_host_command(request.command)) {
    case HostCommand::GetCurrent: {
        if (request.payload.size() != 1) {
            return make_base_response(request, StatusCode::InvalidRequest);
        }

        std::string host;
        std::string path;
        if (!hostState.get_current_host(&host, &path)) {
            return make_base_response(request, StatusCode::DeviceNotFound);
        }

        if (host.size() > 0xFFFFU || path.size() > 0xFFFFU) {
            return make_base_response(request, StatusCode::InternalError);
        }

        write_u8(resp.payload, HOSTPROTO_VERSION);
        write_u16le(resp.payload, static_cast<std::uint16_t>(host.size()));
        write_u16le(resp.payload, static_cast<std::uint16_t>(path.size()));
        resp.payload.insert(resp.payload.end(), host.begin(), host.end());
        resp.payload.insert(resp.payload.end(), path.begin(), path.end());
        return resp;
    }

    case HostCommand::SetCurrent: {
        std::uint16_t specLen = 0;
        if (!read_u16le(request.payload, 1, specLen) ||
            request.payload.size() != 3U + specLen) {
            return make_base_response(request, StatusCode::InvalidRequest);
        }
        const std::string spec(request.payload.begin() + 3, request.payload.end());
        if (!hostState.set_current_host(spec)) {
            return make_base_response(request, StatusCode::IOError);
        }
        resp.payload = {HOSTPROTO_VERSION};
        return resp;
    }

    case HostCommand::ListHistory: {
        std::uint16_t offset = 0;
        std::uint16_t maxBytes = 0;
        if (!read_u16le(request.payload, 1, offset) ||
            !read_u16le(request.payload, 3, maxBytes) ||
            request.payload.size() != 5 ||
            maxBytes == 0) {
            return make_base_response(request, StatusCode::InvalidRequest);
        }

        std::string text;
        if (!hostState.format_history(&text)) {
            return make_base_response(request, StatusCode::IOError);
        }

        const std::size_t start = std::min<std::size_t>(offset, text.size());
        const std::size_t n = std::min<std::size_t>(maxBytes, text.size() - start);
        const bool more = start + n < text.size();

        write_u8(resp.payload, HOSTPROTO_VERSION);
        write_u8(resp.payload, more ? HOST_LIST_MORE : 0);
        write_u16le(resp.payload, offset);
        write_u16le(resp.payload, static_cast<std::uint16_t>(n));
        resp.payload.insert(resp.payload.end(),
                            text.begin() + static_cast<std::ptrdiff_t>(start),
                            text.begin() + static_cast<std::ptrdiff_t>(start + n));
        return resp;
    }

    case HostCommand::SelectHistory: {
        if (request.payload.size() != 2) {
            return make_base_response(request, StatusCode::InvalidRequest);
        }
        if (!hostState.set_current_host_index(request.payload[1])) {
            return make_base_response(request, StatusCode::IOError);
        }
        resp.payload = {HOSTPROTO_VERSION};
        return resp;
    }

    case HostCommand::DeleteHistory: {
        if (request.payload.size() != 2) {
            return make_base_response(request, StatusCode::InvalidRequest);
        }
        if (!hostState.delete_history_index(request.payload[1])) {
            return make_base_response(request, StatusCode::IOError);
        }
        resp.payload = {HOSTPROTO_VERSION};
        return resp;
    }

    default:
        return make_base_response(request, StatusCode::Unsupported);
    }
}

} // namespace fujinet::io
