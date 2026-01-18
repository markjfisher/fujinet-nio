#include "fujinet/io/transport/legacy/bus_hardware.h"
#include "fujinet/core/logging.h"

#include <memory>
#include <thread>
#include <chrono>

namespace fujinet::io::transport::legacy {

static constexpr const char* TAG = "iwm_hw";

// POSIX IWM hardware implementation
// For POSIX, this typically works with SLIP relay or emulation
class IwmHardwarePosix : public BusHardware {
public:
    IwmHardwarePosix() {
        FN_LOGI(TAG, "IwmHardwarePosix created (placeholder)");
    }
    
    ~IwmHardwarePosix() override = default;
    
    void poll() override {
        // No-op for placeholder (no hardware to poll)
    }
    
    bool commandAsserted() const override {
        // For POSIX/SLIP, command assertion is protocol-based
        return false;
    }
    
    bool motorAsserted() const override {
        // For POSIX, motor line not used
        return false;
    }
    
    void setInterrupt(bool level) override {
        // For POSIX, interrupt line not used
        (void)level;
    }
    
    std::size_t read(std::uint8_t* buf, std::size_t len) override {
        // TODO: Read from SLIP relay or serial port
        (void)buf;
        (void)len;
        return 0;
    }
    
    void write(const std::uint8_t* buf, std::size_t len) override {
        // TODO: Write to SLIP relay or serial port
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

std::unique_ptr<BusHardware> make_iwm_hardware() {
    return std::make_unique<IwmHardwarePosix>();
}

} // namespace fujinet::io::transport::legacy
