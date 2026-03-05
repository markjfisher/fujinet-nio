#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/fuji_commands.h"

#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/config/fuji_config.h"

#include <algorithm>

namespace fujinet::io {

using fujinet::config::FujiConfigStore;
using fujinet::config::FujiConfig;
using fujinet::io::protocol::FujiCommand;
using fujinet::io::protocol::to_fuji_command;

FujiDevice::FujiDevice(ResetHandler resetHandler,
                       std::unique_ptr<FujiConfigStore> configStore,
                       fs::StorageManager& storage)
    : _resetHandler(std::move(resetHandler))
    , _configStore(std::move(configStore))
    , _storage(storage)
{
}

void FujiDevice::start()
{
    load_config();
}

IOResponse FujiDevice::handle(const IORequest& request)
{
    auto cmd = to_fuji_command(request.command);

    switch (cmd) {
        case FujiCommand::Reset:
            return handle_reset(request);
        case FujiCommand::GetMounts:
            return handle_get_mounts(request);
        case FujiCommand::SetMount:
            return handle_set_mount(request);
        // later:
        // case FujiCommand::GetSsid:
        //     return handle_get_ssid(request);

        default:
            return handle_unknown(request);
    }
}

IOResponse FujiDevice::handle_get_mounts(const IORequest& request)
{
    IOResponse resp = make_success_response(request);

    // Format: [count][mount1][mount2]...
    // Mount format: [id][uri_len][uri][mode_len][mode]
    std::vector<std::uint8_t> payload;

    // Write mount count
    payload.push_back(static_cast<std::uint8_t>(_config.mounts.size()));

    for (const auto& mount : _config.mounts) {
        // Write slot (1-based for protocol)
        payload.push_back(static_cast<std::uint8_t>(mount.slot));

        // Write URI
        payload.push_back(static_cast<std::uint8_t>(mount.uri.size()));
        payload.insert(payload.end(), mount.uri.begin(), mount.uri.end());

        // Write mode
        payload.push_back(static_cast<std::uint8_t>(mount.mode.size()));
        payload.insert(payload.end(), mount.mode.begin(), mount.mode.end());
    }

    resp.payload = std::move(payload);
    return resp;
}

IOResponse FujiDevice::handle_set_mount(const IORequest& request)
{
    // Format: [id][uri_len][uri][mode_len][mode]
    if (request.payload.size() < 3) { // At least id (1) + uri_len (1) + uri (1) + mode_len (1) + mode (1) = 5? Wait, uri or mode could be empty?
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    std::size_t offset = 0;
    std::uint8_t id = request.payload[offset++];

    std::uint8_t uri_len = request.payload[offset++];
    if (offset + uri_len > request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }
    std::string uri(reinterpret_cast<const char*>(&request.payload[offset]), uri_len);
    offset += uri_len;

    std::uint8_t mode_len = request.payload[offset++];
    if (offset + mode_len > request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }
    std::string mode(reinterpret_cast<const char*>(&request.payload[offset]), mode_len);
    offset += mode_len;

    // Find or create mount
    auto it = std::find_if(_config.mounts.begin(), _config.mounts.end(),
        [id](const fujinet::config::MountConfig& m) { return m.slot == id; });

    if (it != _config.mounts.end()) {
        // Update existing mount
        it->uri = std::move(uri);
        it->mode = std::move(mode);
    } else {
        // Create new mount
        fujinet::config::MountConfig new_mount;
        new_mount.slot = id;
        new_mount.uri = std::move(uri);
        new_mount.mode = std::move(mode);
        _config.mounts.push_back(std::move(new_mount));
    }

    save_config();
    return make_success_response(request);
}

void FujiDevice::poll()
{
    // Background work later (autosave, timers, etc).
}

IOResponse FujiDevice::handle_reset(const IORequest& request)
{
    // We *could* respond first, then reset.
    auto resp = make_success_response(request);

    if (_resetHandler) {
        _resetHandler();
    }

    // On ESP32, reset handler will likely never return.
    return resp;
}

IOResponse FujiDevice::handle_unknown(const IORequest& request)
{
    return make_base_response(request, StatusCode::Unsupported);
}

void FujiDevice::load_config()
{
    if (_configStore) {
        _config = _configStore->load();
    }
}

void FujiDevice::save_config()
{
    if (_configStore) {
        _configStore->save(_config);
    }
}

} // namespace fujinet::io
