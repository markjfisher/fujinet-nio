#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/fuji_commands.h"

#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/io/uri_display_formatter.h"
#include "fujinet/io/devices/app_store.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace {

constexpr std::uint8_t kGetMountsFlagFormatted = 0x01U;
constexpr std::uint8_t kGetMountsResponseFlagMore = 0x01U;
constexpr std::uint8_t kGetMountsResponseFlagFormatted = 0x02U;

struct GetMountsRequest {
    std::uint8_t flags{0};
    int firstSlot{-1};
    int lastSlot{-1};
    std::uint16_t startIndex{0};
    std::uint16_t maxPayloadBytes{0};
};

bool parse_get_mounts_request(const std::vector<std::uint8_t>& payload, GetMountsRequest& out)
{
    if (payload.empty()) {
        return true;
    }

    if (payload.size() != 9) {
        return false;
    }

    out.flags = payload[0];
    out.firstSlot = static_cast<int>(payload[1] | (static_cast<std::uint16_t>(payload[2]) << 8));
    out.lastSlot = static_cast<int>(payload[3] | (static_cast<std::uint16_t>(payload[4]) << 8));
    out.startIndex = static_cast<std::uint16_t>(payload[5] | (static_cast<std::uint16_t>(payload[6]) << 8));
    out.maxPayloadBytes = static_cast<std::uint16_t>(payload[7] | (static_cast<std::uint16_t>(payload[8]) << 8));
    return true;
}

bool mount_record_less(const fujinet::config::MountConfig* lhs, const fujinet::config::MountConfig* rhs)
{
    return lhs->slot < rhs->slot;
}

} // namespace

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
    GetMountsRequest parsed;
    if (!parse_get_mounts_request(request.payload, parsed)) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    IOResponse resp = make_success_response(request);

    std::vector<const fujinet::config::MountConfig*> persisted;
    persisted.reserve(_config.mounts.size());
    for (const auto& mount : _config.mounts) {
        if (mount.slot <= 0) {
            continue;
        }
        persisted.push_back(&mount);
    }

    std::sort(persisted.begin(), persisted.end(), mount_record_less);

    if (request.payload.empty()) {
        // Legacy response: [count][mount1][mount2]... with 0-based slot indices.
        std::vector<std::uint8_t> payload;

        std::vector<const fujinet::config::MountConfig*> legacy;
        legacy.reserve(persisted.size());
        for (const auto* mount : persisted) {
            if (mount->effective_slot() < 0) {
                continue;
            }
            legacy.push_back(mount);
        }

        if (legacy.size() > std::numeric_limits<std::uint8_t>::max()) {
            return make_base_response(request, StatusCode::InternalError);
        }

        payload.push_back(static_cast<std::uint8_t>(legacy.size()));

        for (const auto* mount : legacy) {
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

    if (parsed.maxPayloadBytes == 0) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    if (persisted.empty()) {
        if ((parsed.flags & kGetMountsFlagFormatted) != 0) {
            resp.payload = {0x01U, kGetMountsResponseFlagFormatted, 0x00U, 0x00U, 0x00U,
                            0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
        } else {
            resp.payload = {0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
                            0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
        }
        return resp;
    }

    const int configuredFirst = persisted.front()->slot;
    const int configuredLast = persisted.back()->slot;

    const int firstSlot = (parsed.firstSlot == 0) ? configuredFirst : parsed.firstSlot;
    const int lastSlot = (parsed.lastSlot == 0) ? configuredLast : parsed.lastSlot;

    if (firstSlot <= 0 || lastSlot <= 0 || firstSlot > lastSlot) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    const auto lower = std::lower_bound(
        persisted.begin(), persisted.end(), firstSlot,
        [](const auto* mount, int slot) { return mount->slot < slot; });
    const auto upper = std::upper_bound(
        persisted.begin(), persisted.end(), lastSlot,
        [](int slot, const auto* mount) { return slot < mount->slot; });

    const bool formatted = (parsed.flags & kGetMountsFlagFormatted) != 0;
    const std::size_t total = static_cast<std::size_t>(upper - lower);
    const std::size_t start = (parsed.startIndex < total) ? parsed.startIndex : total;

    std::vector<std::uint8_t> payload;

    // Extended response header:
    // [version][flags][first_slot_le16][start_index_le16][entry_count_le16][entries_len_le16][data...]
    payload.reserve(10 + parsed.maxPayloadBytes);
    payload.push_back(0x01U);
    payload.push_back(0x00U);
    payload.push_back(static_cast<std::uint8_t>(firstSlot & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((firstSlot >> 8) & 0xFF));
    payload.push_back(static_cast<std::uint8_t>(parsed.startIndex & 0xFF));
    payload.push_back(static_cast<std::uint8_t>((parsed.startIndex >> 8) & 0xFF));

    const std::size_t entryCountOffset = payload.size();
    payload.push_back(0x00U);
    payload.push_back(0x00U);
    const std::size_t entriesLenOffset = payload.size();
    payload.push_back(0x00U);
    payload.push_back(0x00U);

    std::uint16_t entryCount = 0;
    if (formatted) {
        for (std::size_t i = start; i < total; ++i) {
            const auto* mount = *(lower + static_cast<std::ptrdiff_t>(i));
            const std::string line = format_mount_line(
                formatted_mount_slot_label(*mount),
                mount->uri,
                mount->mode,
                mount->enabled);
            const std::size_t entriesUsed = payload.size() - (entriesLenOffset + 2);
            if (entriesUsed + line.size() > parsed.maxPayloadBytes) {
                break;
            }
            payload.insert(payload.end(), line.begin(), line.end());
            ++entryCount;
        }
    } else {
        for (std::size_t i = start; i < total; ++i) {
            const auto* mount = *(lower + static_cast<std::ptrdiff_t>(i));
            if (mount->slot > std::numeric_limits<std::uint16_t>::max()) {
                return make_base_response(request, StatusCode::InternalError);
            }
            if (mount->uri.size() > std::numeric_limits<std::uint8_t>::max() ||
                mount->mode.size() > std::numeric_limits<std::uint8_t>::max()) {
                return make_base_response(request, StatusCode::InternalError);
            }

            const std::size_t entryBytes = 2U + 1U + 1U + mount->uri.size() + mount->mode.size();
            const std::size_t entriesUsed = payload.size() - (entriesLenOffset + 2);
            if (entriesUsed + entryBytes > parsed.maxPayloadBytes) {
                break;
            }

            payload.push_back(static_cast<std::uint8_t>(mount->slot & 0xFF));
            payload.push_back(static_cast<std::uint8_t>((mount->slot >> 8) & 0xFF));
            payload.push_back(mount->enabled ? 0x01U : 0x00U);
            payload.push_back(static_cast<std::uint8_t>(mount->uri.size()));
            payload.insert(payload.end(), mount->uri.begin(), mount->uri.end());
            payload.push_back(static_cast<std::uint8_t>(mount->mode.size()));
            payload.insert(payload.end(), mount->mode.begin(), mount->mode.end());
            ++entryCount;
        }
    }

    const std::size_t entriesLen = payload.size() - (entriesLenOffset + 2);
    const bool more = (start + entryCount < total);

    std::uint8_t responseFlags = 0;
    if (more) {
        responseFlags |= kGetMountsResponseFlagMore;
    }
    if (formatted) {
        responseFlags |= kGetMountsResponseFlagFormatted;
    }
    payload[1] = responseFlags;

    payload[entryCountOffset] = static_cast<std::uint8_t>(entryCount & 0xFF);
    payload[entryCountOffset + 1] = static_cast<std::uint8_t>((entryCount >> 8) & 0xFF);
    payload[entriesLenOffset] = static_cast<std::uint8_t>(entriesLen & 0xFF);
    payload[entriesLenOffset + 1] = static_cast<std::uint8_t>((entriesLen >> 8) & 0xFF);

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

    // Empty/clear records must still provide the full 4-byte header
    // [slot][flags][uri_len][mode_len]. Reject any truncated or overlong form
    // before indexing into the payload.
    const std::uint8_t uri_len = request.payload[offset++];
    if (offset + uri_len > request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }
    std::string uri;
    if (uri_len > 0) {
        uri.assign(reinterpret_cast<const char*>(request.payload.data() + offset), uri_len);
    }
    offset += uri_len;

    if (offset >= request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    const std::uint8_t mode_len = request.payload[offset++];
    if (offset + mode_len > request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }
    std::string mode;
    if (mode_len > 0) {
        mode.assign(reinterpret_cast<const char*>(request.payload.data() + offset), mode_len);
    }
    offset += mode_len;

    if (offset != request.payload.size()) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }

    if (mode.empty()) {
        mode = "r";
    }

    const int slotNumber = fujinet::config::MountConfig::from_index(slotIndex);
    const bool enabled = (flags & 0x01U) != 0 && !uri.empty();

    if (!uri.empty()) {
        AppStore store(_storage);
        std::string canonical_uri;
        if (!store.resolve_target(uri, canonical_uri, nullptr)) {
            return make_base_response(request, StatusCode::InvalidRequest);
        }
        uri = std::move(canonical_uri);
    }

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

fujinet::config::MountConfig* FujiDevice::find_mount_by_slot_number(int slotNumber)
{
    auto it = std::find_if(_config.mounts.begin(), _config.mounts.end(),
        [slotNumber](const fujinet::config::MountConfig& m) { return m.slot == slotNumber; });
    return (it == _config.mounts.end()) ? nullptr : &(*it);
}

const fujinet::config::MountConfig* FujiDevice::find_mount_by_slot_number(int slotNumber) const
{
    auto it = std::find_if(_config.mounts.begin(), _config.mounts.end(),
        [slotNumber](const fujinet::config::MountConfig& m) { return m.slot == slotNumber; });
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

int FujiDevice::formatted_mount_slot_label(const fujinet::config::MountConfig& mount)
{
    const int slotIndex = mount.effective_slot();
    return (slotIndex >= 0) ? slotIndex : mount.slot;
}

std::string FujiDevice::format_mount_line(int slotLabel,
                                          std::string_view uri,
                                          std::string_view mode,
                                          bool enabled)
{
    const auto display = format_uri_for_display(uri);

    std::string line;
    line.reserve(display.summary.size() + display.detail.size() + mode.size() + 40);
    line += std::to_string(slotLabel);
    line += enabled ? ":* " : ":  ";
    if (!mode.empty()) {
        line += "[";
        for (char ch : mode) {
            line.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        line += "] ";
    }
    line += display.summary;
    line.push_back('\n');
    if (!display.detail.empty()) {
        line += "  path: ";
        line += display.detail;
        line.push_back('\n');
    }
    return line;
}

} // namespace fujinet::io
