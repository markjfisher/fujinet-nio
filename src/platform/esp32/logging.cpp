#include "fujinet/core/logging.h"

#if !defined(FN_DEBUG)

// Non-debug build: nothing here. Inline stubs in the header handle calls.

#else

#include <cstdio>
#include <string>

extern "C" {
#include "esp_log.h"
}

namespace fujinet::log {

static const char* to_esp_tag(const char* tag)
{
    return tag ? tag : "log";
}

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
    }

    va_end(args);
}

} // namespace fujinet::log


#endif // FN_DEBUG
