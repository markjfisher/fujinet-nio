#pragma once

#include "fujinet/io/core/channel.h"

#include <vector>
#include <cstdint>

extern "C" {
#include "driver/uart.h"
struct QueueDefinition;
typedef struct QueueDefinition* QueueHandle_t;
}

namespace fujinet::platform::esp32 {

/// Channel implementation for UART over GPIO on ESP32.
/// Used for UartGpio channel kind (SIO, RS232, etc.).
/// Pin configuration is obtained from the pinmap at construction time.
///
/// This implementation uses an event queue and internal FIFO to ensure
/// reliable data reception even when the polling interval is longer than
/// the time between incoming bytes.
class UartChannel : public fujinet::io::Channel {
public:
    UartChannel();
    ~UartChannel() override;

    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;

    /// Process pending UART events and update internal FIFO.
    /// Should be called regularly (e.g., from poll() or serviceOnce()).
    /// This is automatically called from available() and read().
    void updateFIFO();

    /// Flush output buffer, waiting for transmission to complete.
    void flushOutput();

    /// Get current baud rate.
    uint32_t getBaudrate();

    /// Set baud rate.
    void setBaudrate(uint32_t baud);

private:
    bool initialize();
    bool _initialized{false};
    
    // UART port number
    uart_port_t _uart_port{UART_NUM_1};
    
    // Event queue for UART events
    QueueHandle_t _uart_queue{nullptr};
    
    // Internal FIFO for received data
    std::vector<std::uint8_t> _fifo;
};

} // namespace fujinet::platform::esp32
