#pragma once

#include <functional>
#include <memory>

#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/config/fuji_config.h"


namespace fujinet::io {

class FujiDevice : public VirtualDevice {
public:
    using ResetHandler = std::function<void()>;

    FujiDevice(ResetHandler resetHandler,
               std::unique_ptr<fujinet::config::FujiConfigStore> configStore,
               fs::StorageManager& storage);

    IOResponse handle(const IORequest& request) override;
    void       poll() override;

private:
    IOResponse handle_reset(const IORequest& request);
    IOResponse handle_unknown(const IORequest& request);

    IOResponse make_base_response(const IORequest& request,
                                  StatusCode status = StatusCode::Ok) const;

    void load_config();
    void save_config();

private:
    ResetHandler                                        _resetHandler;
    std::unique_ptr<fujinet::config::FujiConfigStore>   _configStore;
    fujinet::config::FujiConfig                         _config;
    fs::StorageManager&                                 _storage;
};

} // namespace fujinet::io
