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

void early_logf(const char* fmt, ...);

#if defined(FN_DEBUG)

// Real functions exist only in debug builds.
void vlogf(Level level, const char* tag, const char* fmt, std::va_list args);
void logf(Level level, const char* tag, const char* fmt, ...);
void log(Level level, const char* tag, std::string_view message);

#else

// In non-debug builds, provide inline no-op stubs so
// any direct calls from core still compile but vanish.
inline void vlogf(Level, const char*, const char*, std::va_list) {}

template <typename... Args>
inline void logf(Level, const char*, const char*, Args&&...) {}

inline void log(Level, const char*, std::string_view) {}

#endif // FN_DEBUG

} // namespace fujinet::log

// ------------------------------------------------------------------
// Convenience macros
// ------------------------------------------------------------------


#if defined(FN_DEBUG)

#define FN_ELOG(fmt, ...) ::fujinet::log::early_logf(fmt "\n", ##__VA_ARGS__)

#define FN_LOGE(tag, fmt, ...) \
    ::fujinet::log::logf(::fujinet::log::Level::Error,   tag, fmt, ##__VA_ARGS__)

#define FN_LOGW(tag, fmt, ...) \
    ::fujinet::log::logf(::fujinet::log::Level::Warn,    tag, fmt, ##__VA_ARGS__)

#define FN_LOGI(tag, fmt, ...) \
    ::fujinet::log::logf(::fujinet::log::Level::Info,    tag, fmt, ##__VA_ARGS__)

#define FN_LOGD(tag, fmt, ...) \
    ::fujinet::log::logf(::fujinet::log::Level::Debug,   tag, fmt, ##__VA_ARGS__)

#define FN_LOGV(tag, fmt, ...) \
    ::fujinet::log::logf(::fujinet::log::Level::Verbose, tag, fmt, ##__VA_ARGS__)

#else

// In non-debug builds they compile to a single no-op expression.
// The whole macro invocation (including arguments / fmt strings)
// disappears at preprocessing time, so no strings or code remain.

#define FN_LOGE(tag, fmt, ...) ((void)0)
#define FN_LOGW(tag, fmt, ...) ((void)0)
#define FN_LOGI(tag, fmt, ...) ((void)0)
#define FN_LOGD(tag, fmt, ...) ((void)0)
#define FN_LOGV(tag, fmt, ...) ((void)0)

#endif // FN_DEBUG
