#include "fujinet/core/logging.h"

#if !defined(FN_DEBUG)

// In non-debug builds, we provide *no* implementation here.
// The inline stubs in the header are enough, and the linker
// will have nothing to pull in from this TU.

#else

#include <cstdio>
#include <string>

extern "C" {
#include "esp_log.h"
}

namespace fujinet::log {

static esp_log_level_t to_esp_level(Level lvl)
{
    switch (lvl) {
    case Level::Error:   return ESP_LOG_ERROR;
    case Level::Warn:    return ESP_LOG_WARN;
    case Level::Info:    return ESP_LOG_INFO;
    case Level::Debug:   return ESP_LOG_DEBUG;
    case Level::Verbose: return ESP_LOG_VERBOSE;
    }
    return ESP_LOG_INFO;
}

// Debug-only, so we can afford heap + two passes with vsnprintf
void vlogf(Level level, const char* tag, const char* fmt, std::va_list args)
{
    if (!fmt) {
        return;
    }

    esp_log_level_t esp_level = to_esp_level(level);
    const char*     esp_tag   = tag ? tag : "log";

    // First pass: compute required size
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed < 0) {
        return;
    }

    // Allocate buffer (+2 for optional '\n' and '\0')
    std::string buf;
    buf.resize(static_cast<std::size_t>(needed));

    // Second pass: actually format
    va_list args_copy2;
    va_copy(args_copy2, args);
    std::vsnprintf(buf.data(), buf.size() + 1, fmt, args_copy2);
    va_end(args_copy2);

    // Ensure newline at end
    if (buf.empty() || buf.back() != '\n') {
        buf.push_back('\n');
    }

    // esp_log_write prints the message as-is, no extra newline.
    esp_log_write(esp_level, esp_tag, "%s", buf.c_str());
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
    // Preformatted string variant
    logf(level, tag, "%.*s",
         static_cast<int>(message.size()),
         message.data());
}

} // namespace fujinet::log

#endif // FN_DEBUG
