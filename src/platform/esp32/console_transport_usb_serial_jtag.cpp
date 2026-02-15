// Console transport using ESP32-S3's built-in USB Serial JTAG.
// This is the internal USB-serial converter (NOT external JTAG hardware).
// Used when CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG is set.

#include "fujinet/console/console_engine.h"

#include <string>
#include <string_view>

extern "C" {
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace fujinet::console {

namespace {

class Esp32UsbSerialJtagConsoleTransport final : public IConsoleTransport {
public:
    Esp32UsbSerialJtagConsoleTransport()
    {
        // Initialize the USB Serial JTAG driver for console I/O.
        // Even though CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG is set,
        // the driver may not be initialized for our use.
        usb_serial_jtag_driver_config_t cfg = {};
        cfg.tx_buffer_size = 256;
        cfg.rx_buffer_size = 256;
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        (void)err;  // Ignore errors - may already be installed
    }

    bool read_byte(std::uint8_t& out, int timeout_ms) override
    {
        // usb_serial_jtag_read_bytes returns immediately if no data,
        // so we need to handle timeout ourselves with polling.
        uint8_t byte;
        
        // First, try an immediate read (handles timeout_ms=0 case)
        int n = usb_serial_jtag_read_bytes(&byte, 1, 0);
        if (n == 1) {
            out = byte;
            return true;
        }
        
        // If no data and timeout is 0, return immediately
        if (timeout_ms == 0) {
            return false;
        }
        
        // Poll with delay for positive timeout
        int total_waited = 0;
        const int poll_interval_ms = 10;
        
        while (timeout_ms < 0 || total_waited < timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
            total_waited += poll_interval_ms;
            
            n = usb_serial_jtag_read_bytes(&byte, 1, 0);
            if (n == 1) {
                out = byte;
                return true;
            }
        }
        return false;
    }

    void write(std::string_view s) override
    {
        if (!s.empty()) {
            usb_serial_jtag_write_bytes(s.data(), static_cast<size_t>(s.size()), 0);
        }
    }

    void write_line(std::string_view s) override
    {
        write(s);
        write("\r\n");
    }

private:
};

} // namespace

std::unique_ptr<IConsoleTransport> create_console_transport_usb_serial_jtag()
{
    return std::make_unique<Esp32UsbSerialJtagConsoleTransport>();
}

} // namespace fujinet::console
