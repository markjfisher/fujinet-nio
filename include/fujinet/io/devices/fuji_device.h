#pragma once

#include <functional>
#include <memory>

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/virtual_device.h"

namespace fujinet::io {

class FujiDevice : public VirtualDevice {
public:
    using ResetHandler = std::function<void()>;

    FujiDevice(ResetHandler resetHandler,
               std::unique_ptr<fujinet::config::FujiConfigStore> configStore);

    IOResponse handle(const IORequest& request) override;
    void       poll() override;

    // Phase-1 bring-up (non-critical path)
    void start();

    const fujinet::config::FujiConfig& config() const { return _config; }

    using HsioGet = std::function<int()>;
    using HsioSet = std::function<bool(int)>;
    void set_hsio_handlers(HsioGet get, HsioSet set) { _hsioGet = std::move(get); _hsioSet = std::move(set); }
    
    /// Provide mutable access to config for devices that need to modify it (e.g., HostService)
    fujinet::config::FujiConfig& config_mut() { return _config; }
    
    /// Provide access to the config store for other devices that need persistence
    config::FujiConfigStore* config_store() { return _configStore.get(); }

private:
    IOResponse handle_get_info(const IORequest& request);
    IOResponse handle_reset(const IORequest& request);
    IOResponse handle_unknown(const IORequest& request);
    void load_config();

private:
    ResetHandler                                      _resetHandler;
    HsioGet                                           _hsioGet;
    HsioSet                                           _hsioSet;
    std::unique_ptr<fujinet::config::FujiConfigStore> _configStore;
    fujinet::config::FujiConfig                       _config;
};

} // namespace fujinet::io
