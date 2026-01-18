#include "fujinet/io/transport/legacy/bus_hardware.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/core/logging.h"

#include <memory>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <string>

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "sio_hw";

// Forward declaration
std::unique_ptr<BusHardware> make_netsio_bus_hardware(Channel& channel, const config::NetSioConfig& config);

// POSIX SIO hardware implementation
// For POSIX, this typically works with NetSIO or a serial port
// This is a placeholder that can be extended for actual hardware
class SioHardwarePosix : public BusHardware {
public:
    SioHardwarePosix() {
        FN_LOGI(TAG, "SioHardwarePosix created (placeholder)");
    }
    
    ~SioHardwarePosix() override = default;
    
    bool commandAsserted() const override {
        // For POSIX/NetSIO, command assertion is handled by the protocol
        // Return false for now (no command)
        return false;
    }
    
    bool motorAsserted() const override {
        // For POSIX, motor line not typically used
        return false;
    }
    
    void setInterrupt(bool level) override {
        // For POSIX, interrupt line not typically used
        (void)level;
    }
    
    std::size_t read(std::uint8_t* buf, std::size_t len) override {
        // TODO: Read from serial port or NetSIO
        (void)buf;
        (void)len;
        return 0;
    }
    
    void write(const std::uint8_t* buf, std::size_t len) override {
        // TODO: Write to serial port or NetSIO
        (void)buf;
        (void)len;
    }
    
    void write(std::uint8_t byte) override {
        write(&byte, 1);
    }
    
    void flush() override {
        // TODO: Flush serial port output
    }
    
    std::size_t available() const override {
        // TODO: Check serial port available bytes
        return 0;
    }
    
    void discardInput() override {
        // TODO: Discard serial port input buffer
    }
    
    void delayMicroseconds(std::uint32_t us) override {
        // POSIX delay using std::this_thread::sleep_for
        std::this_thread::sleep_for(std::chrono::microseconds(us));
    }
};

std::unique_ptr<BusHardware> make_sio_hardware(
    Channel* channel,
    const config::NetSioConfig* netsioConfig
) {
    // Check if we should use NetSIO hardware (UDP channel + config enabled)
    bool useNetsio = false;
    config::NetSioConfig netsioCfg;
    
    if (channel && netsioConfig) {
        useNetsio = netsioConfig->enabled;
        netsioCfg = *netsioConfig;
    } else if (channel) {
        // No config provided, check environment variables as fallback
        const char* host_env = std::getenv("NETSIO_HOST");
        const char* port_env = std::getenv("NETSIO_PORT");
        
        if (host_env || port_env) {
            useNetsio = true;
            netsioCfg.enabled = true;
            netsioCfg.host = host_env ? host_env : "localhost";
            netsioCfg.port = 9997;
            
            if (port_env) {
                try {
                    int p = std::stoi(port_env);
                    if (p > 0 && p <= 65535) {
                        netsioCfg.port = static_cast<std::uint16_t>(p);
                    }
                } catch (...) {
                    // Invalid port, use default
                }
            }
        }
    }
    
    if (useNetsio && channel) {
        return make_netsio_bus_hardware(*channel, netsioCfg);
    }
    
    // Otherwise, use regular POSIX hardware (placeholder for now)
    return std::make_unique<SioHardwarePosix>();
}

} // namespace fujinet::io::transport::legacy
