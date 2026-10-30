#pragma once

#include <cstdint>

namespace fujinet::io {

// A serial line whose speed can change while it is open. Platform channels
// that drive a real UART or serial port implement it beside io::Channel; it is
// not part of io::Channel. The Palm cradle (hotsync::CradleChannel) uses it to
// lend the bus line at another rate.
class BaudControl {
public:
    virtual ~BaudControl() = default;

    virtual std::uint32_t baud_rate() const = 0;

    // Waits for pending output to leave the line, then switches. False when
    // the rate is not supported.
    virtual bool set_baud_rate(std::uint32_t baud) = 0;
};

} // namespace fujinet::io
