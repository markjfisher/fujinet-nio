#pragma once

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/platform/esp32/pinmap.h"

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
    /// Host-facing UART from `channel.uart` in FujiConfig (baud, framing, optional RTS/CTS).
    explicit UartChannel(const config::UartConfig& uart_cfg = {});
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

    /// Set baud rate (updates hardware and stored `UartConfig`).
    void setBaudrate(uint32_t baud);

    /// Last applied settings (may differ slightly from hardware baud rounding).
    const config::UartConfig& uart_config() const { return _uart_cfg; }

    /// Re-apply framing/baud/flow on the live UART (driver must already be installed).
    /// Flushes RX/TX first. Returns false if pins are invalid or ESP-IDF calls fail.
    bool reconfigure(const config::UartConfig& cfg);

private:
    bool initialize();
    /// Apply `uart_param_config` + `uart_set_pin` for current `_uart_cfg`.
    bool apply_hw_parameters(const UartPins& uart_pins);
    bool _initialized{false};
    config::UartConfig _uart_cfg{};
    
    // UART port number
    uart_port_t _uart_port{UART_NUM_1};
    
    // Event queue for UART events
    QueueHandle_t _uart_queue{nullptr};
    
    // Internal FIFO for received data
    std::vector<std::uint8_t> _fifo;
};

} // namespace fujinet::platform::esp32
