#include "fujinet/platform/esp32/uart_channel.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/esp32/pinmap.h"

#include <cstddef>
#include <algorithm>

extern "C" {
#include "driver/uart.h"
#include "soc/uart_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
}

namespace fujinet::platform::esp32 {

static const char* TAG = "uart_channel";

// UART configuration
static constexpr int UART_BAUD_RATE = 115200;
static constexpr int UART_RX_BUF_SIZE = 2048;
static constexpr int UART_TX_BUF_SIZE = 0;  // 0 = TX buffer not used, blocking write
static constexpr int UART_QUEUE_SIZE = 10;
static constexpr int MAX_FLUSH_WAIT_TICKS = 200;

UartChannel::UartChannel()
{
    _initialized = initialize();
    if (!_initialized) {
        FN_LOGE(TAG, "Failed to initialize UartChannel");
    }
}

UartChannel::~UartChannel()
{
    if (_initialized) {
        uart_driver_delete(_uart_port);
        _initialized = false;
    }
    
    // Queue is deleted by uart_driver_delete, but clear our reference
    _uart_queue = nullptr;
}

bool UartChannel::initialize()
{
    // Get UART pins from the pinmap
    const UartPins uart_pins = pinmap().primaryUart();
    
    if (uart_pins.rx < 0 || uart_pins.tx < 0) {
        FN_LOGE(TAG, "No valid UART pins configured in pinmap");
        return false;
    }

    FN_LOGI(TAG, "Using UART pins: RX=%d, TX=%d", uart_pins.rx, uart_pins.tx);

    // Use UART_NUM_1 for the channel (UART_NUM_0 is typically the debug console)
    _uart_port = UART_NUM_1;

    // Configure UART
    uart_config_t uart_config = {};
    uart_config.baud_rate = UART_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_param_config(_uart_port, &uart_config);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "uart_param_config failed: %d", err);
        return false;
    }

    // Set UART pins
    err = uart_set_pin(_uart_port, uart_pins.tx, uart_pins.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "uart_set_pin failed: %d", err);
        return false;
    }

    // Install UART driver with event queue
    // Using a queue allows us to receive UART events (data ready, break, errors, etc.)
    err = uart_driver_install(
        _uart_port,
        UART_RX_BUF_SIZE,
        UART_TX_BUF_SIZE,
        UART_QUEUE_SIZE,
        &_uart_queue,
        0        // intr_alloc_flags
    );
    if (err != ESP_OK) {
        FN_LOGE(TAG, "uart_driver_install failed: %d", err);
        return false;
    }

    FN_LOGI(TAG, "UartChannel initialized on UART%d with event queue", _uart_port);
    return true;
}

void UartChannel::updateFIFO()
{
    if (!_initialized || !_uart_queue) {
        return;
    }

    uart_event_t event;

    // Process all pending events from the queue
    // Use 1 tick timeout to avoid blocking if queue is empty
    while (xQueueReceive(_uart_queue, &event, 1))
    {
        switch (event.type) {
        case UART_DATA:
            // Data is available in the UART hardware FIFO
            // Read it into our internal FIFO
            {
                size_t old_len = _fifo.size();
                _fifo.resize(old_len + event.size);
                int result = uart_read_bytes(_uart_port, &_fifo[old_len], event.size, 0);
                if (result < 0) {
                    result = 0;
                }
                _fifo.resize(old_len + result);
            }
            break;

        case UART_FIFO_OVF:
            // UART FIFO overflow - data was lost
            FN_LOGW(TAG, "UART FIFO overflow");
            uart_flush_input(_uart_port);
            break;

        case UART_BUFFER_FULL:
            // Ring buffer full - this shouldn't happen with our config
            FN_LOGW(TAG, "UART buffer full");
            uart_flush_input(_uart_port);
            break;

        case UART_BREAK:
            // Break condition detected
            FN_LOGI(TAG, "UART break detected");
            break;

        case UART_PARITY_ERR:
            FN_LOGW(TAG, "UART parity error");
            break;

        case UART_FRAME_ERR:
            FN_LOGW(TAG, "UART frame error");
            break;

        case UART_PATTERN_DET:
            // Pattern detected (if pattern detection is enabled)
            break;

        default:
            FN_LOGW(TAG, "Unknown UART event: %d", event.type);
            break;
        }
    }
}

bool UartChannel::available()
{
    if (!_initialized) {
        return false;
    }

    // First, process any pending events to update our FIFO
    updateFIFO();

    // Return true if we have data in our internal FIFO
    return !_fifo.empty();
}

std::size_t UartChannel::read(std::uint8_t* buffer, std::size_t maxLen)
{
    if (!_initialized || buffer == nullptr || maxLen == 0) {
        return 0;
    }

    // Process any pending events first
    updateFIFO();

    // Read from our internal FIFO
    std::size_t to_copy = (maxLen < _fifo.size()) ? maxLen : _fifo.size();
    if (to_copy == 0) {
        return 0;
    }

    // Copy data to caller's buffer
    std::copy(_fifo.begin(), _fifo.begin() + to_copy, buffer);

    // Remove the copied data from our FIFO
    _fifo.erase(_fifo.begin(), _fifo.begin() + to_copy);

    return to_copy;
}

void UartChannel::write(const std::uint8_t* buffer, std::size_t len)
{
    if (!_initialized || buffer == nullptr || len == 0) {
        return;
    }

    int bytes_written = uart_write_bytes(_uart_port, buffer, len);
    if (bytes_written < 0) {
        FN_LOGE(TAG, "uart_write_bytes failed: %d", bytes_written);
        return;
    }

    if (static_cast<std::size_t>(bytes_written) != len) {
        FN_LOGW(TAG, "Partial write: %d of %zu bytes", bytes_written, len);
    }
}

void UartChannel::flushOutput()
{
    if (!_initialized) {
        return;
    }
    uart_wait_tx_done(_uart_port, MAX_FLUSH_WAIT_TICKS);
}

uint32_t UartChannel::getBaudrate()
{
    if (!_initialized) {
        return 0;
    }
    uint32_t baud = 0;
    uart_get_baudrate(_uart_port, &baud);
    return baud;
}

void UartChannel::setBaudrate(uint32_t baud)
{
    if (!_initialized) {
        return;
    }
    uart_set_baudrate(_uart_port, baud);
}

} // namespace fujinet::platform::esp32
