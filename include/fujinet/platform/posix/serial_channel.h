#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <termios.h>

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/channel.h"

namespace fujinet::platform::posix {

struct SerialSettings {
    std::string port;
    config::UartConfig uart{.baudRate = 19200};
};

bool is_supported_serial_baud(std::uint32_t baud);
std::uint32_t effective_serial_baud(std::uint32_t requested);
termios make_serial_termios(const config::UartConfig& uart);
SerialSettings resolve_serial_settings(const config::FujiConfig& config);

std::unique_ptr<fujinet::io::Channel>
create_serial_channel_for_path(const std::string& port, const config::UartConfig& uart);

std::unique_ptr<fujinet::io::Channel> create_serial_channel(const config::FujiConfig& config);

} // namespace fujinet::platform::posix
