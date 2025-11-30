#include "fujinet/platform/esp32/channel_factory.h"

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/config/build_profile.h"

extern "C" {
#include "driver/uart.h"
#include "esp_log.h"
}

namespace fujinet::platform::esp32 {

static const char* TAG = "esp32-channel";

// Simple UART-backed Channel.
// For now, we use UART_NUM_0 which is usually connected to the USB-serial
// bridge on dev boards, so you can talk to it via /dev/ttyACM*.
class UartChannel : public fujinet::io::Channel {
public:
    explicit UartChannel(uart_port_t port)
        : _port(port)
    {}

    bool available() override {
        size_t buffered = 0;
        if (uart_get_buffered_data_len(_port, &buffered) == ESP_OK) {
            return buffered > 0;
        }
        return false;
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override {
        if (maxLen == 0) {
            return 0;
        }

        int n = uart_read_bytes(
            _port,
            buffer,
            maxLen,
            0  // ticks_to_wait = 0 for non-blocking
        );

        if (n <= 0) {
            return 0;
        }
        return static_cast<std::size_t>(n);
    }

    void write(const std::uint8_t* buffer, std::size_t len) override {
        if (!buffer || len == 0) {
            return;
        }

        int n = uart_write_bytes(
            _port,
            reinterpret_cast<const char*>(buffer),
            len
        );
        (void)n; // ignoring short writes for now
    }

private:
    uart_port_t _port;
};

static std::unique_ptr<fujinet::io::Channel> create_uart0_channel()
{
    // Basic UART configuration; adjust baud rate etc. as needed.
    const int uart_num = UART_NUM_0;
    uart_config_t cfg{};
    cfg.baud_rate  = 115200;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;

    esp_err_t err = uart_param_config(uart_num, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %d", err);
        return nullptr;
    }

    // On many ESP32-S3 dev boards, UART0 pins are pre-wired to USB-serial.
    // If needed, you can change these to custom pins.
    err = uart_set_pin(
        uart_num,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %d", err);
        return nullptr;
    }

    // Install driver with RX/TX buffers.
    err = uart_driver_install(
        uart_num,
        1024,  // rx_buffer_size
        1024,  // tx_buffer_size
        0,     // event queue size
        nullptr,
        0      // flags
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %d", err);
        return nullptr;
    }

    ESP_LOGI(TAG, "UartChannel created on UART0 @ 115200 baud");
    return std::make_unique<UartChannel>(static_cast<uart_port_t>(uart_num));
}

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const config::BuildProfile& profile)
{
    using config::TransportKind;

    switch (profile.primaryTransport) {
    case TransportKind::SerialDebug:
        ESP_LOGW(TAG, "TransportKind::SerialDebug requested on ESP32, going to UART0");
        return create_uart0_channel();

    case TransportKind::SIO:
    case TransportKind::IEC:
        // TODO: Implement SIO/IEC-specific channel when ready.
        ESP_LOGE(TAG, "SIO/IEC channel not implemented on ESP32 yet");
        return nullptr;
    }

    return nullptr;
}

} // namespace fujinet::platform::esp32
