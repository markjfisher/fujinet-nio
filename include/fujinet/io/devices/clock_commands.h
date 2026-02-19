#pragma once

#include <cstdint>

namespace fujinet::io {

// Clock device commands
enum class ClockCommand : std::uint8_t {
    GetTime         = 0x01,  // Get raw Unix time (existing)
    SetTime         = 0x02,  // Set raw Unix time (existing)
    GetTimeFormat   = 0x03,  // Get time in specified format
    GetTimezone     = 0x04,  // Get current timezone string
    SetTimezone     = 0x05,  // Set timezone (non-persistent)
    SetTimezoneSave = 0x06,  // Set timezone and persist to config
};

// Time format codes (must match fujinet-nio-lib FnTimeFormat enum)
// These values match the legacy fujinet-lib TimeFormat enum for compatibility.
enum class TimeFormat : std::uint8_t {
    // Binary formats - raw bytes, not ASCII
    SimpleBinary    = 0x00,  // 7 bytes: [century, year, month, day, hour, min, sec]
    ProDOSBinary    = 0x01,  // 4 bytes: Apple ProDOS format
    ApeTimeBinary   = 0x02,  // 6 bytes: [day, month, year, hour, min, sec]
    
    // String formats - null-terminated ASCII
    TzIsoString     = 0x03,  // ISO 8601 with TZ offset: "YYYY-MM-DDTHH:MM:SS+HHMM"
    UtcIsoString    = 0x04,  // ISO 8601 UTC: "YYYY-MM-DDTHH:MM:SS+0000"
    
    // Apple III SOS format
    Apple3SosBinary = 0x05,  // 16 bytes: "YYYYMMDD0HHMMSS000"
};

// Clock protocol version
static constexpr std::uint8_t CLOCK_PROTO_VERSION = 1;

// Maximum timezone string length
static constexpr std::size_t MAX_TIMEZONE_LEN = 64;

// Maximum formatted time string length
static constexpr std::size_t MAX_TIME_STRING_LEN = 32;

// Binary format sizes
static constexpr std::size_t TIME_FORMAT_SIMPLE_SIZE = 7;
static constexpr std::size_t TIME_FORMAT_PRODOS_SIZE = 4;
static constexpr std::size_t TIME_FORMAT_APETIME_SIZE = 6;
static constexpr std::size_t TIME_FORMAT_SOS_SIZE = 16;

} // namespace fujinet::io
