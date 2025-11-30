#pragma once

#include "fujinet/io/core/channel.h"

namespace fujinet::platform::esp32 {

class UsbCdcChannel : public fujinet::io::Channel {
public:
    UsbCdcChannel();
    ~UsbCdcChannel() override = default;

    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;
};

} // namespace fujinet::platform::esp32
