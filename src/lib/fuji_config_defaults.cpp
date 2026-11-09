#include "fujinet/config/fuji_config_defaults.h"

namespace fujinet::config {

FujiConfig default_config_for(const build::BuildProfile& profile)
{
    FujiConfig cfg{};
    if (!profile.defaultBootConfigUri.empty()) {
        cfg.boot.configUri = std::string(profile.defaultBootConfigUri);
    }
    if (profile.defaultChannelTcpPort != 0) {
        cfg.channel.tcpPort = profile.defaultChannelTcpPort;
    }
    if (profile.defaultUartBaudRate != 0) {
        cfg.channel.uart.baudRate = profile.defaultUartBaudRate;
    }
    if (profile.defaultUartUnpaced) {
        cfg.channel.uart.txChunkSize = 0;
        cfg.channel.uart.txChunkGapUs = 0;
    }
    return cfg;
}

} // namespace fujinet::config
