#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/fuji_commands.h"

#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/core/logging.h"

extern "C" {
#include "cJSON.h"
}

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
    load_config();
}

IOResponse FujiDevice::make_base_response(const IORequest& request,
                                          StatusCode status) const
{
    IOResponse resp;
    resp.id       = request.id;
    resp.deviceId = request.deviceId;
    resp.command  = request.command;
    resp.status   = status;
    return resp;
}

IOResponse FujiDevice::handle(const IORequest& request)
{
    auto cmd = to_fuji_command(request.command);

    switch (cmd) {
        case FujiCommand::Reset:
            return handle_reset(request);

        // later:
        // case FujiCommand::GetSsid:
        //     return handle_get_ssid(request);

        default:
            return handle_unknown(request);
    }
}

void FujiDevice::poll()
{
    // Background work later (autosave, timers, etc).
}

IOResponse FujiDevice::handle_reset(const IORequest& request)
{
    // We *could* respond first, then reset.
    auto resp = make_base_response(request, StatusCode::Ok);

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
