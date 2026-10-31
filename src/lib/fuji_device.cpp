#include "fujinet/io/devices/fuji_device.h"

#include "fujinet/build/profile.h"
#include "fujinet/core/version.h"
#include "fujinet/io/devices/fuji_commands.h"

#include <algorithm>
#include <string_view>

namespace fujinet::io {

using fujinet::config::FujiConfig;
using fujinet::config::FujiConfigStore;
using fujinet::io::protocol::FujiCommand;
using fujinet::io::protocol::to_fuji_command;

namespace {

constexpr std::uint8_t INFO_VERSION = 1;
constexpr std::size_t MAX_FIRMWARE_VERSION = 32;
constexpr std::size_t MAX_PROFILE_NAME = 64;

void put_string8(std::vector<std::uint8_t>& out, std::string_view text, std::size_t max)
{
    text = text.substr(0, max);
    out.push_back(static_cast<std::uint8_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

} // namespace

FujiDevice::FujiDevice(ResetHandler resetHandler,
                       std::unique_ptr<FujiConfigStore> configStore)
    : _resetHandler(std::move(resetHandler))
    , _configStore(std::move(configStore))
{
}

IOResponse FujiDevice::handle(const IORequest& request)
{
    switch (to_fuji_command(request.command)) {
        case FujiCommand::GetInfo:
            return handle_get_info(request);
        case FujiCommand::GetHsio:
        case FujiCommand::SetHsio: {
            if (request.payload.empty() || request.payload[0] == 0 ||
                (request.command == static_cast<std::uint8_t>(FujiCommand::SetHsio) &&
                 (request.payload.size() < 2 || (request.payload[1] > 9 && request.payload[1] != 0xff))))
                return make_base_response(request, StatusCode::InvalidRequest);
            if (!_hsioGet || !_hsioSet) return make_base_response(request, StatusCode::Unsupported);
            if (request.command == static_cast<std::uint8_t>(FujiCommand::SetHsio)) {
                const int index = request.payload[1] == 0xff ? -1 : request.payload[1];
                if (!_hsioSet(index)) return make_base_response(request, StatusCode::InvalidRequest);
            }
            const int index = _hsioGet();
            auto response = make_success_response(request);
            response.payload = {1, static_cast<std::uint8_t>(index < 0 ? 0xff : index)};
            return response;
        }
        case FujiCommand::Reset:
            return handle_reset(request);
        default:
            return handle_unknown(request);
    }
}

void FujiDevice::poll()
{
    // Background work later (autosave, timers, etc).
}

void FujiDevice::start()
{
    load_config();
}

IOResponse FujiDevice::handle_get_info(const IORequest& request)
{
    // Extendable command (protocol_reference.md, "Extending commands"): reply
    // in the lower of the client's version and ours, ignoring trailing bytes.
    if (request.payload.empty() || request.payload[0] == 0) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }
    auto resp = make_success_response(request);
    resp.payload = {std::min(request.payload[0], INFO_VERSION)};
    put_string8(resp.payload, fujinet::version(), MAX_FIRMWARE_VERSION);
    put_string8(resp.payload, build::current_build_profile().name, MAX_PROFILE_NAME);
    return resp;
}

IOResponse FujiDevice::handle_reset(const IORequest& request)
{
    auto resp = make_success_response(request);

    if (_resetHandler) {
        _resetHandler();
    }

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

} // namespace fujinet::io
