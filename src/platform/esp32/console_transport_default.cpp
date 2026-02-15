#include "fujinet/console/console_engine.h"

extern "C" {
#include "sdkconfig.h"
}

#include "fujinet/core/logging.h"

namespace fujinet::console {

static const char* TAG = "console";

// Implemented in console_transport_uart.cpp (only referenced when needed).
#if !CONFIG_FN_CONSOLE_TRANSPORT_USB_CDC || CONFIG_FN_CONSOLE_ALLOW_UART_FALLBACK
std::unique_ptr<IConsoleTransport> create_console_transport_uart0();
#endif

// Implemented in console_transport_stdio.cpp when secondary is USB Serial JTAG.
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
std::unique_ptr<IConsoleTransport> create_console_transport_stdio();
#endif

// Implemented in console_transport_usbcdc.cpp
std::unique_ptr<IConsoleTransport> create_console_transport_usbcdc();

std::unique_ptr<IConsoleTransport> create_default_console_transport()
{
#if !CONFIG_FN_CONSOLE_ENABLE
    return nullptr;
#else
#if CONFIG_FN_CONSOLE_TRANSPORT_USB_CDC
    // USB CDC requested.
    auto cdc = create_console_transport_usbcdc();
    if (cdc) {
        return cdc;
    }
#if CONFIG_FN_CONSOLE_ALLOW_UART_FALLBACK
    FN_LOGW(TAG, "Console configured for USB CDC but unavailable; falling back to UART0");
    return create_console_transport_uart0();
#else
    FN_LOGW(TAG, "Console configured for USB CDC but unavailable; console disabled (UART fallback disabled)");
    return nullptr;
#endif
#else
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    // Logs go to USB Serial JTAG and we did not install UART0; use stdin/stdout for CLI.
    return create_console_transport_stdio();
#else
    return create_console_transport_uart0();
#endif
#endif
#endif
}

} // namespace fujinet::console


