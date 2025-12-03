#include "fujinet/core/logging.h"

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

void vlogf(Level level, const char* tag, const char* fmt, std::va_list args)
{
    esp_log_level_t esp_level = to_esp_level(level);
    esp_log_writev(esp_level, tag ? tag : "log", fmt, args);
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
