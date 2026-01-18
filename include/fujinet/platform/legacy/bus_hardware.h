#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

namespace fujinet::platform::legacy {

// Abstract interface for platform-specific hardware access
// This allows transports to work with hardware without platform-specific code
class BusHardware {
public:
    virtual ~BusHardware() = default;
    
    // GPIO operations
    virtual bool commandAsserted() const = 0;
    virtual bool motorAsserted() const = 0;
    virtual void setInterrupt(bool level) = 0;
    
    // Serial/UART operations
    virtual std::size_t read(std::uint8_t* buf, std::size_t len) = 0;
    virtual void write(const std::uint8_t* buf, std::size_t len) = 0;
    virtual void write(std::uint8_t byte) = 0;
    virtual void flush() = 0;
    virtual std::size_t available() const = 0;
    virtual void discardInput() = 0;
    
    // Timing
    virtual void delayMicroseconds(std::uint32_t us) = 0;
};

// Platform-specific factories
std::unique_ptr<BusHardware> make_sio_hardware();

} // namespace fujinet::platform::legacy
