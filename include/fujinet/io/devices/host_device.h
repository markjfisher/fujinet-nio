#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/virtual_device.h"

namespace fujinet::io {

// HostDevice: Provides access to host slot configuration.
// Wire device ID: WireDeviceId::HostService (0xF0).
//
// This device allows clients (like the BBC Micro b2-rom) to query and
// configure host slots, which represent storage backends (SD card, TNFS
// servers, etc.) that disk images can be mounted from.
//
// The host configuration is persisted via FujiConfigStore.
class HostDevice : public VirtualDevice {
public:
    explicit HostDevice(config::FujiConfig& config);

    IOResponse handle(const IORequest& request) override;
    void poll() override {}

private:
    static constexpr std::uint8_t HOSTPROTO_VERSION = 1;
    static constexpr std::size_t MAX_HOSTS = 8;
    static constexpr std::size_t MAX_HOST_NAME_LEN = 32;
    static constexpr std::size_t MAX_HOST_ADDR_LEN = 64;

    IOResponse handle_get_hosts(const IORequest& request);
    IOResponse handle_set_host(const IORequest& request);
    IOResponse handle_get_host(const IORequest& request);
    IOResponse handle_unknown(const IORequest& request);

    config::FujiConfig& _config;
};

} // namespace fujinet::io
