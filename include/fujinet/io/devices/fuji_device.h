#pragma once

#include <functional>
#include <memory>
#include <string>

#include "fujinet/config/fuji_config.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/virtual_device.h"

namespace fujinet::io {

class FujiDevice : public VirtualDevice {
public:
    using ResetHandler = std::function<void()>;

    FujiDevice(ResetHandler resetHandler,
               std::unique_ptr<fujinet::config::FujiConfigStore> configStore,
               fs::StorageManager& storage);

    IOResponse handle(const IORequest& request) override;
    void       poll() override;

    // Phase-1 bring-up (non-critical path)
    void start();

    const fujinet::config::FujiConfig& config() const { return _config; }
    
    /// Provide mutable access to config for devices that need to modify it (e.g., HostDevice)
    fujinet::config::FujiConfig& config_mut() { return _config; }
    
    /// Provide access to the config store for other devices that need persistence
    config::FujiConfigStore* config_store() { return _configStore.get(); }

private:
    IOResponse handle_reset(const IORequest& request);
    IOResponse handle_unknown(const IORequest& request);
    IOResponse handle_get_mounts(const IORequest& request);
    IOResponse handle_get_mount(const IORequest& request);
    IOResponse handle_set_mount(const IORequest& request);

    config::MountConfig* find_mount_by_slot_number(int slot);
    const config::MountConfig* find_mount_by_slot_number(int slot) const;
    static bool is_valid_mount_slot_index(std::uint8_t slotIndex);
    static std::vector<std::uint8_t> encode_mount_record(std::uint8_t slotIndex,
                                                         const std::string& uri,
                                                         const std::string& mode,
                                                         bool enabled);

    void load_config();
    void save_config();

private:
    ResetHandler                                      _resetHandler;
    std::unique_ptr<fujinet::config::FujiConfigStore> _configStore;
    fujinet::config::FujiConfig                       _config;
    fs::StorageManager&                               _storage;
};

} // namespace fujinet::io
