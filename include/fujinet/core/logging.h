#pragma once

#include <cstdarg>
#include <string_view>

namespace fujinet::log {

enum class Level {
    Error = 0,
    Warn,
    Info,
    Debug,
    Verbose,
};

// Implemented per-platform in src/platform/<platform>/logging.cpp
void vlogf(Level level, const char* tag, const char* fmt, std::va_list args);
void logf(Level level, const char* tag, const char* fmt, ...);
void log(Level level, const char* tag, std::string_view message);

// Convenience wrappers
inline void error(const char* tag, std::string_view msg) {
    log(Level::Error, tag, msg);
}
inline void warn(const char* tag, std::string_view msg) {
    log(Level::Warn, tag, msg);
}
inline void info(const char* tag, std::string_view msg) {
    log(Level::Info, tag, msg);
}
inline void debug(const char* tag, std::string_view msg) {
    log(Level::Debug, tag, msg);
}
inline void verbose(const char* tag, std::string_view msg) {
    log(Level::Verbose, tag, msg);
}

// printf-style macros mirroring ESP-IDF style
#define FN_LOGE(TAG, FMT, ...) ::fujinet::log::logf(::fujinet::log::Level::Error,   TAG, FMT, ##__VA_ARGS__)
#define FN_LOGW(TAG, FMT, ...) ::fujinet::log::logf(::fujinet::log::Level::Warn,    TAG, FMT, ##__VA_ARGS__)
#define FN_LOGI(TAG, FMT, ...) ::fujinet::log::logf(::fujinet::log::Level::Info,    TAG, FMT, ##__VA_ARGS__)
#define FN_LOGD(TAG, FMT, ...) ::fujinet::log::logf(::fujinet::log::Level::Debug,   TAG, FMT, ##__VA_ARGS__)
#define FN_LOGV(TAG, FMT, ...) ::fujinet::log::logf(::fujinet::log::Level::Verbose, TAG, FMT, ##__VA_ARGS__)

} // namespace fujinet::log
