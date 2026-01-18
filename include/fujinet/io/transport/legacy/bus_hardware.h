#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

namespace fujinet::config {
    struct NetSioConfig;
}

namespace fujinet::io {
    class Channel;
}

namespace fujinet::io::transport::legacy {

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
    
    // NetSIO-specific: check if sync response is needed and send it
    // Returns true if sync response was sent, false otherwise
    // Default implementation returns false (no sync needed)
    virtual bool sendSyncResponseIfNeeded(std::uint8_t ackByte, std::uint16_t writeSize = 0) {
        (void)ackByte;
        (void)writeSize;
        return false;
    }
};

// Platform-specific factories (implemented in platform-specific files)
// For SIO, pass channel and optional config (config used for NetSIO mode)
std::unique_ptr<BusHardware> make_sio_hardware(
    Channel* channel = nullptr,
    const config::NetSioConfig* netsioConfig = nullptr
);
std::unique_ptr<BusHardware> make_iwm_hardware();

} // namespace fujinet::io::transport::legacy
