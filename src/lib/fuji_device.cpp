#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/fuji_commands.h"

namespace fujinet::io {

using fujinet::config::FujiConfig;
using fujinet::config::FujiConfigStore;
using fujinet::io::protocol::FujiCommand;
using fujinet::io::protocol::to_fuji_command;

FujiDevice::FujiDevice(ResetHandler resetHandler,
                       std::unique_ptr<FujiConfigStore> configStore)
    : _resetHandler(std::move(resetHandler))
    , _configStore(std::move(configStore))
{
}

IOResponse FujiDevice::handle(const IORequest& request)
{
    switch (to_fuji_command(request.command)) {
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
