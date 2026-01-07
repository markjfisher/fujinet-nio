#include "fujinet/console/console_engine.h"

#include <string>

extern "C" {
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace fujinet::console {

namespace {

class Esp32UartConsoleTransport final : public IConsoleTransport {
public:
    Esp32UartConsoleTransport()
    {
        const uart_port_t port = UART_NUM_0;

        // Install driver if not already present. If it's already installed,
        // uart_driver_install() returns ESP_FAIL; we ignore that.
        (void)uart_driver_install(port, 1024, 0, 0, nullptr, 0);

        uart_config_t cfg = {};
        cfg.baud_rate = 115200;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;

        (void)uart_param_config(port, &cfg);
        (void)uart_set_pin(port, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    bool read_line(std::string& out, int timeout_ms) override
    {
        const uart_port_t port = UART_NUM_0;

        std::uint8_t tmp[64];
        const TickType_t to = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

        int n = uart_read_bytes(port, tmp, sizeof(tmp), to);
        if (n <= 0) {
            return false;
        }

        for (int i = 0; i < n; ++i) {
            const char ch = static_cast<char>(tmp[i]);
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                out = _buf;
                _buf.clear();
                return true;
            }
            _buf.push_back(ch);

            // Basic safety bound (avoid unbounded growth if no newline).
            if (_buf.size() > 512) {
                _buf.clear();
                out.clear();
                return true;
            }
        }

        return false;
    }

    void write(std::string_view s) override
    {
        const uart_port_t port = UART_NUM_0;
        (void)uart_write_bytes(port, s.data(), static_cast<size_t>(s.size()));
    }

    void write_line(std::string_view s) override
    {
        write(s);
        write("\r\n");
    }

private:
    std::string _buf;
};

} // namespace

std::unique_ptr<IConsoleTransport> create_console_transport_uart0()
{
    return std::make_unique<Esp32UartConsoleTransport>();
}

} // namespace fujinet::console


