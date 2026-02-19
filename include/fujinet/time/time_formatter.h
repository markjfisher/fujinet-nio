#pragma once

#include "fujinet/io/devices/clock_commands.h"
#include "fujinet/platform/time.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fujinet::time {

// Import TimeFormat from io namespace for convenience
using fujinet::io::TimeFormat;

/**
 * @brief Time formatting service for converting Unix timestamps to various formats.
 * 
 * This service provides platform-agnostic time formatting functions that convert
 * Unix timestamps to various binary and string formats used by different host systems.
 * All formatting is done on the FujiNet device to offload work from resource-constrained
 * 8-bit hosts.
 */
class TimeFormatter {
public:
    /**
     * @brief Format a Unix timestamp into the specified format using the given timezone.
     * 
     * @param unix_seconds Unix timestamp (seconds since 1970-01-01 00:00:00 UTC)
     * @param format The desired output format
     * @param timezone POSIX timezone string (e.g., "UTC" or "CET-1CEST,M3.5.0,M10.5.0/3")
     * @return std::vector<uint8_t> Formatted time data (binary or string depending on format)
     * @throws std::invalid_argument if timezone is invalid
     */
    static std::vector<std::uint8_t> format_time(
        std::uint64_t unix_seconds,
        TimeFormat format,
        const std::string& timezone);
    
    /**
     * @brief Format time as Simple Binary (7 bytes).
     * 
     * Format: [century, year, month, day, hour, minute, second]
     * - century: e.g., 20 for years 2000-2099
     * - year: year within century, e.g., 24 for 2024
     * - month: 1-12
     * - day: 1-31
     * - hour: 0-23
     * - minute: 0-59
     * - second: 0-59
     */
    static std::vector<std::uint8_t> format_simple(const platform::LocalTime& lt);
    
    /**
     * @brief Format time as ProDOS Binary (4 bytes).
     * 
     * ProDOS format reference: https://prodos8.com/docs/techref/adding-routines-to-prodos/
     * 
     * Byte 0: day + (month << 5)
     * Byte 1: (year % 100 << 1) + (month >> 3)
     * Byte 2: minute
     * Byte 3: hour
     */
    static std::vector<std::uint8_t> format_prodos(const platform::LocalTime& lt);
    
    /**
     * @brief Format time as ApeTime Binary (6 bytes).
     * 
     * Atari ApeTime format:
     * - Byte 0: day (1-31)
     * - Byte 1: month (1-12)
     * - Byte 2: year - 2000 (e.g., 24 for 2024)
     * - Byte 3: hour (0-23)
     * - Byte 4: minute (0-59)
     * - Byte 5: second (0-59)
     */
    static std::vector<std::uint8_t> format_apetime(const platform::LocalTime& lt);
    
    /**
     * @brief Format time as Apple III SOS Binary (16 bytes).
     * 
     * Format: "YYYYMMDD0HHMMSS000" (ASCII digits, null-padded)
     * Reference: SOS Reference Manual
     */
    static std::vector<std::uint8_t> format_apple3_sos(const platform::LocalTime& lt);
    
    /**
     * @brief Format time as ISO 8601 string with timezone offset.
     * 
     * Format: "YYYY-MM-DDTHH:MM:SS+HHMM" (null-terminated)
     * Example: "2024-01-15T14:30:00+0100"
     */
    static std::string format_iso8601_tz(std::uint64_t unix_seconds, const std::string& timezone);
    
    /**
     * @brief Format time as ISO 8601 UTC string.
     * 
     * Format: "YYYY-MM-DDTHH:MM:SS+0000" (null-terminated)
     * Example: "2024-01-15T13:30:00+0000"
     */
    static std::string format_iso8601_utc(std::uint64_t unix_seconds);
};

} // namespace fujinet::time
