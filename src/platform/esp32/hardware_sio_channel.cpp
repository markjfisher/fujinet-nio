#include "fujinet/platform/esp32/hardware_sio_channel.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/esp32/pinmap.h"

#include <cstddef>

extern "C" {
#include "driver/uart.h"
#include "soc/uart_struct.h"
}

namespace fujinet::platform::esp32 {

static const char* TAG = "hw_sio_channel";

// UART configuration for SIO/RS232
static constexpr uart_port_t UART_PORT = UART_NUM_1;
static constexpr int UART_BAUD_RATE = 115200;
static constexpr int UART_RX_BUF_SIZE = 2048;
static constexpr int UART_TX_BUF_SIZE = 2048;
static constexpr int UART_TIMEOUT_MS = 20;

HardwareSioChannel::HardwareSioChannel()
{
    _initialized = initialize();
    if (!_initialized) {
        FN_LOGE(TAG, "Failed to initialize HardwareSioChannel");
    }
}

HardwareSioChannel::~HardwareSioChannel()
{
    if (_initialized) {
        uart_driver_delete(UART_PORT);
        _initialized = false;
    }
}

bool HardwareSioChannel::initialize()
{
    const auto& pins = pinmap();

    // Determine which pin set to use based on configuration
    // For RS232 builds, use rs232 pins; for SIO builds, use sio pins
    int rx_pin = -1;
    int tx_pin = -1;

    // Prefer RS232 pins if configured (for FN_BUILD_ESP32_FUJIBUS_GPIO)
    if (pins.rs232.uart_rx >= 0 && pins.rs232.uart_tx >= 0) {
        rx_pin = pins.rs232.uart_rx;
        tx_pin = pins.rs232.uart_tx;
        FN_LOGI(TAG, "Using RS232 pins: RX=%d, TX=%d", rx_pin, tx_pin);
    }
    // Fall back to SIO UART pins if available (for FN_BUILD_ATARI_SIO)
    else if (pins.sio.uart_rx >= 0 && pins.sio.uart_tx >= 0) {
        rx_pin = pins.sio.uart_rx;
        tx_pin = pins.sio.uart_tx;
        FN_LOGI(TAG, "Using SIO UART pins: RX=%d, TX=%d", rx_pin, tx_pin);
    }
    else {
        FN_LOGE(TAG, "No valid UART pins configured in pinmap");
        return false;
    }

    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(UART_PORT, &uart_config);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "uart_param_config failed: %d", err);
        return false;
    }

    // Set UART pins
    err = uart_set_pin(UART_PORT, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "uart_set_pin failed: %d", err);
        return false;
    }

    // Install UART driver
    err = uart_driver_install(
        UART_PORT,
        UART_RX_BUF_SIZE,
        UART_TX_BUF_SIZE,
        0,      // queue size
        nullptr, // queue handle
        0       // intr_alloc_flags
    );
    if (err != ESP_OK) {
        FN_LOGE(TAG, "uart_driver_install failed: %d", err);
        return false;
    }

    FN_LOGI(TAG, "HardwareSioChannel initialized on UART%d", UART_PORT);
    return true;
}

bool HardwareSioChannel::available()
{
    if (!_initialized) {
        return false;
    }

    size_t bytes_available = 0;
    esp_err_t err = uart_get_buffered_data_len(UART_PORT, &bytes_available);
    if (err != ESP_OK) {
        return false;
    }

    return bytes_available > 0;
}

std::size_t HardwareSioChannel::read(std::uint8_t* buffer, std::size_t maxLen)
{
    if (!_initialized || buffer == nullptr || maxLen == 0) {
        return 0;
    }

    int bytes_read = uart_read_bytes(
        UART_PORT,
        buffer,
        maxLen,
        UART_TIMEOUT_MS / portTICK_PERIOD_MS
    );

    if (bytes_read < 0) {
        FN_LOGE(TAG, "uart_read_bytes failed: %d", bytes_read);
        return 0;
    }

    return static_cast<std::size_t>(bytes_read);
}

void HardwareSioChannel::write(const std::uint8_t* buffer, std::size_t len)
{
    if (!_initialized || buffer == nullptr || len == 0) {
        return;
    }

    int bytes_written = uart_write_bytes(UART_PORT, buffer, len);
    if (bytes_written < 0) {
        FN_LOGE(TAG, "uart_write_bytes failed: %d", bytes_written);
        return;
    }

    if (static_cast<std::size_t>(bytes_written) != len) {
        FN_LOGW(TAG, "Partial write: %d of %zu bytes", bytes_written, len);
    }
}

} // namespace fujinet::platform::esp32
