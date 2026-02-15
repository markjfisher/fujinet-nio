#include "fujinet/core/logging.h"

#include <cstdio>
#include <cstdarg>
#include <string>

extern "C" {
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"
}

#if CONFIG_TINYUSB_CDC_ENABLED
#include "fujinet/platform/esp32/tinyusb_cdc.h"
#endif

namespace fujinet::log {

static const char* to_esp_tag(const char* tag)
{
    return tag ? tag : "log";
}

void early_logf(const char* fmt, ...)
{
    if (!fmt) return;

    std::va_list args;
    va_start(args, fmt);

    // Very early / fast path; avoids ESP_LOG entirely.
    // esp_rom_printf doesn't support va_list directly, so we format to a small stack buffer.
    char buf[256];
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0) {
        esp_rom_printf("%s", buf);
#if CONFIG_TINYUSB_CDC_ENABLED
        // When log output is CDC, also send early lines there (port inited in early_init).
        const auto port = (CONFIG_FN_ESP_CONSOLE_CDC_NUM == 0)
            ? fujinet::platform::esp32::UsbCdcAcmPort::Port0
            : fujinet::platform::esp32::UsbCdcAcmPort::Port1;
        std::size_t len = static_cast<std::size_t>(n);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        fujinet::platform::esp32::write_cdc_port(port, buf, len);
#endif
    }

    va_end(args);
}

#if !defined(FN_DEBUG)

// Non-debug build: nothing else here. Inline stubs in the header handle calls.

#else

// Debug-only, so we can afford heap + two-pass vsnprintf.
void vlogf(Level level, const char* tag, const char* fmt, std::va_list args)
{
    if (!fmt) {
        return;
    }

    const char* esp_tag = to_esp_tag(tag);

    // First pass: determine needed size
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed < 0) {
        return;
    }

    // Allocate buffer (no +1 trickery; std::string manages the null)
    std::string buf;
    buf.resize(static_cast<std::size_t>(needed));

    // Second pass: actually format into the buffer
    va_list args_copy2;
    va_copy(args_copy2, args);
    std::vsnprintf(buf.data(), buf.size() + 1, fmt, args_copy2);
    va_end(args_copy2);

    // DO NOT append '\n' here. ESP_LOGx already terminates lines.

    switch (level) {
    case Level::Error:
        ESP_LOGE(esp_tag, "%s", buf.c_str());
        break;
    case Level::Warn:
        ESP_LOGW(esp_tag, "%s", buf.c_str());
        break;
    case Level::Info:
        ESP_LOGI(esp_tag, "%s", buf.c_str());
        break;
    case Level::Debug:
        ESP_LOGD(esp_tag, "%s", buf.c_str());
        break;
    case Level::Verbose:
        ESP_LOGV(esp_tag, "%s", buf.c_str());
        break;
    }
}

void logf(Level level, const char* tag, const char* fmt, ...)
{
    std::va_list args;
    va_start(args, fmt);
    vlogf(level, tag, fmt, args);
    va_end(args);
}

void log(Level level, const char* tag, std::string_view message)
{
    logf(level, tag, "%.*s",
         static_cast<int>(message.size()),
         message.data());
}

#endif // FN_DEBUG

} // namespace fujinet::log