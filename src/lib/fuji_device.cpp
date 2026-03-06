#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/fuji_commands.h"

#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/config/fuji_config.h"

#include <algorithm>
#include <limits>

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
        case FujiCommand::GetMount:
            return handle_get_mount(request);
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
    // Mount format: [slot_index][flags][uri_len][uri][mode_len][mode]
    std::vector<std::uint8_t> payload;

    std::vector<const fujinet::config::MountConfig*> persisted;
    persisted.reserve(_config.mounts.size());
    for (const auto& mount : _config.mounts) {
        if (mount.effective_slot() < 0) {
            continue;
        }
        persisted.push_back(&mount);
    }

    std::sort(persisted.begin(), persisted.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->slot < rhs->slot;
    });

    if (persisted.size() > std::numeric_limits<std::uint8_t>::max()) {
        return make_base_response(request, StatusCode::InternalError);
    }

    payload.push_back(static_cast<std::uint8_t>(persisted.size()));

    for (const auto* mount : persisted) {
        auto record = encode_mount_record(
            static_cast<std::uint8_t>(mount->effective_slot()),
            mount->uri,
            mount->mode,
            mount->enabled);
        payload.insert(payload.end(), record.begin(), record.end());
    }

    resp.payload = std::move(payload);
    return resp;
}

IOResponse FujiDevice::handle_get_mount(const IORequest& request)
{
    if (request.payload.size() != 1) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    const std::uint8_t slotIndex = request.payload[0];
    if (!is_valid_mount_slot_index(slotIndex)) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    IOResponse resp = make_success_response(request);
    const int slotNumber = fujinet::config::MountConfig::from_index(slotIndex);
    const auto* mount = find_mount_by_slot_number(slotNumber);
    if (!mount) {
        resp.payload = encode_mount_record(slotIndex, "", "r", false);
        return resp;
    }

    resp.payload = encode_mount_record(slotIndex, mount->uri, mount->mode, mount->enabled);
    return resp;
}

IOResponse FujiDevice::handle_set_mount(const IORequest& request)
{
    // Format: [slot_index][flags][uri_len][uri][mode_len][mode]
    if (request.payload.size() < 4) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    std::size_t offset = 0;
    const std::uint8_t slotIndex = request.payload[offset++];
    const std::uint8_t flags = request.payload[offset++];

    if (!is_valid_mount_slot_index(slotIndex)) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    const std::uint8_t uri_len = request.payload[offset++];
    if (offset + uri_len > request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }
    std::string uri(reinterpret_cast<const char*>(&request.payload[offset]), uri_len);
    offset += uri_len;

    if (offset >= request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    const std::uint8_t mode_len = request.payload[offset++];
    if (offset + mode_len > request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }
    std::string mode(reinterpret_cast<const char*>(&request.payload[offset]), mode_len);
    offset += mode_len;

    if (offset != request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    if (mode.empty()) {
        mode = "r";
    }

    const int slotNumber = fujinet::config::MountConfig::from_index(slotIndex);
    const bool enabled = (flags & 0x01U) != 0 && !uri.empty();

    auto* mount = find_mount_by_slot_number(slotNumber);
    if (uri.empty()) {
        if (mount) {
            _config.mounts.erase(std::remove_if(_config.mounts.begin(), _config.mounts.end(),
                                    [slotNumber](const fujinet::config::MountConfig& m) { return m.slot == slotNumber; }),
                                _config.mounts.end());
        }
    } else {
        if (!mount) {
            fujinet::config::MountConfig new_mount;
            new_mount.slot = slotNumber;
            new_mount.uri = std::move(uri);
            new_mount.mode = std::move(mode);
            new_mount.enabled = enabled;
            _config.mounts.push_back(std::move(new_mount));
        } else {
            mount->uri = std::move(uri);
            mount->mode = std::move(mode);
            mount->enabled = enabled;
        }
    }

    std::sort(_config.mounts.begin(), _config.mounts.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.slot < rhs.slot;
    });

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

fujinet::config::MountConfig* FujiDevice::find_mount_by_slot_number(int slot)
{
    auto it = std::find_if(_config.mounts.begin(), _config.mounts.end(),
        [slot](const fujinet::config::MountConfig& m) { return m.slot == slot; });
    return (it == _config.mounts.end()) ? nullptr : &(*it);
}

const fujinet::config::MountConfig* FujiDevice::find_mount_by_slot_number(int slot) const
{
    auto it = std::find_if(_config.mounts.begin(), _config.mounts.end(),
        [slot](const fujinet::config::MountConfig& m) { return m.slot == slot; });
    return (it == _config.mounts.end()) ? nullptr : &(*it);
}

bool FujiDevice::is_valid_mount_slot_index(std::uint8_t slotIndex)
{
    return slotIndex < 8;
}

std::vector<std::uint8_t> FujiDevice::encode_mount_record(std::uint8_t slotIndex,
                                                          const std::string& uri,
                                                          const std::string& mode,
                                                          bool enabled)
{
    if (uri.size() > std::numeric_limits<std::uint8_t>::max() ||
        mode.size() > std::numeric_limits<std::uint8_t>::max()) {
        return {};
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(4 + uri.size() + mode.size());
    payload.push_back(slotIndex);
    payload.push_back(enabled ? 0x01U : 0x00U);
    payload.push_back(static_cast<std::uint8_t>(uri.size()));
    payload.insert(payload.end(), uri.begin(), uri.end());
    payload.push_back(static_cast<std::uint8_t>(mode.size()));
    payload.insert(payload.end(), mode.begin(), mode.end());
    return payload;
}

} // namespace fujinet::io
