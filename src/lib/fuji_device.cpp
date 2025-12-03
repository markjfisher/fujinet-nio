#include "fujinet/io/devices/fuji_device.h"

namespace fujinet::io {

using fujinet::config::FujiConfigStore;
using fujinet::config::FujiConfig;

using protocol::FujiCommandId;
using protocol::FujiDeviceId;
using protocol::to_fuji_command;

FujiDevice::FujiDevice(ResetHandler resetHandler,
                       std::unique_ptr<FujiConfigStore> configStore)
    : _resetHandler(std::move(resetHandler))
    , _configStore(std::move(configStore))
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
        case FujiCommandId::Reset:
            return handle_reset(request);

        // later:
        // case FujiCommandId::GetSsid:
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
