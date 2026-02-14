#pragma once

#include "fujinet/io/core/channel.h"

namespace fujinet::platform::esp32 {

/// Channel implementation for hardware SIO/UART on ESP32.
/// Used for FujiBus over GPIO (e.g., RS232) builds.
class HardwareSioChannel : public fujinet::io::Channel {
public:
    HardwareSioChannel();
    ~HardwareSioChannel() override;

    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;

private:
    bool initialize();
    bool _initialized{false};
};

} // namespace fujinet::platform::esp32
