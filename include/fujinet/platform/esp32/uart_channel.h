#pragma once

#include "fujinet/io/core/channel.h"

namespace fujinet::platform::esp32 {

/// Channel implementation for UART over GPIO on ESP32.
/// Used for UartGpio channel kind (SIO, RS232, etc.).
/// Pin configuration is obtained from the pinmap at construction time.
class UartChannel : public fujinet::io::Channel {
public:
    UartChannel();
    ~UartChannel() override;

    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;

private:
    bool initialize();
    bool _initialized{false};
};

} // namespace fujinet::platform::esp32
