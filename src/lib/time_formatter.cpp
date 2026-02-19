#include "fujinet/time/time_formatter.h"

#include <stdexcept>
#include <cstdio>
#include <cstring>

namespace fujinet::time {

using fujinet::io::TimeFormat;

std::vector<std::uint8_t> TimeFormatter::format_time(
    std::uint64_t unix_seconds,
    TimeFormat format,
    const std::string& timezone)
{
    // For UTC formats, we don't need local time conversion
    if (format == TimeFormat::UtcIsoString) {
        std::string result = format_iso8601_utc(unix_seconds);
        return std::vector<std::uint8_t>(result.begin(), result.end());
    }
    
    // For timezone formats, we need local time
    platform::LocalTime lt;
    if (!platform::get_local_time(unix_seconds, timezone.c_str(), lt)) {
        throw std::invalid_argument("Invalid timezone: " + timezone);
    }
    
    switch (format) {
        case TimeFormat::SimpleBinary:
            return format_simple(lt);
            
        case TimeFormat::ProDOSBinary:
            return format_prodos(lt);
            
        case TimeFormat::ApeTimeBinary:
            return format_apetime(lt);
            
        case TimeFormat::TzIsoString: {
            std::string result = format_iso8601_tz(unix_seconds, timezone);
            return std::vector<std::uint8_t>(result.begin(), result.end());
        }
        
        case TimeFormat::Apple3SosBinary:
            return format_apple3_sos(lt);
            
        default:
            throw std::invalid_argument("Unknown time format");
    }
}

std::vector<std::uint8_t> TimeFormatter::format_simple(const platform::LocalTime& lt)
{
    std::vector<std::uint8_t> result(7);
    
    // Century (e.g., 20 for 2024)
    result[0] = static_cast<std::uint8_t>(lt.year / 100);
    
    // Year within century (e.g., 24 for 2024)
    result[1] = static_cast<std::uint8_t>(lt.year % 100);
    
    // Month (1-12)
    result[2] = static_cast<std::uint8_t>(lt.month);
    
    // Day (1-31)
    result[3] = static_cast<std::uint8_t>(lt.day);
    
    // Hour (0-23)
    result[4] = static_cast<std::uint8_t>(lt.hour);
    
    // Minute (0-59)
    result[5] = static_cast<std::uint8_t>(lt.minute);
    
    // Second (0-59)
    result[6] = static_cast<std::uint8_t>(lt.second);
    
    return result;
}

std::vector<std::uint8_t> TimeFormatter::format_prodos(const platform::LocalTime& lt)
{
    // ProDOS time format (4 bytes):
    // https://prodos8.com/docs/techref/adding-routines-to-prodos/
    //
    //         7 6 5 4 3 2 1 0   7 6 5 4 3 2 1 0
    //         +-+-+-+-+-+-+-+-+ +-+-+-+-+-+-+-+-+
    //  DATE:  |    year     |  month  |   day   |
    //         +-+-+-+-+-+-+-+-+ +-+-+-+-+-+-+-+-+
    //         7 6 5 4 3 2 1 0   7 6 5 4 3 2 1 0
    //         +-+-+-+-+-+-+-+-+ +-+-+-+-+-+-+-+-+
    //  TIME:  |    hour       | |    minute     |
    //         +-+-+-+-+-+-+-+-+ +-+-+-+-+-+-+-+-+
    
    std::vector<std::uint8_t> result(4);
    
    int year = lt.year % 100;  // Year within century (0-99)
    int month = lt.month;       // 1-12
    int day = lt.day;           // 1-31
    int hour = lt.hour;         // 0-23
    int minute = lt.minute;     // 0-59
    
    // DATE low byte: day + (month << 5)
    result[0] = static_cast<std::uint8_t>(day + (month << 5));
    
    // DATE high byte: (year << 1) + (month >> 3)
    result[1] = static_cast<std::uint8_t>((year << 1) + (month >> 3));
    
    // TIME low byte: minute
    result[2] = static_cast<std::uint8_t>(minute);
    
    // TIME high byte: hour
    result[3] = static_cast<std::uint8_t>(hour);
    
    return result;
}

std::vector<std::uint8_t> TimeFormatter::format_apetime(const platform::LocalTime& lt)
{
    // ApeTime format (6 bytes):
    // [day, month, year-2000, hour, minute, second]
    
    std::vector<std::uint8_t> result(6);
    
    result[0] = static_cast<std::uint8_t>(lt.day);
    result[1] = static_cast<std::uint8_t>(lt.month);
    result[2] = static_cast<std::uint8_t>(lt.year - 2000);  // e.g., 24 for 2024
    result[3] = static_cast<std::uint8_t>(lt.hour);
    result[4] = static_cast<std::uint8_t>(lt.minute);
    result[5] = static_cast<std::uint8_t>(lt.second);
    
    return result;
}

std::vector<std::uint8_t> TimeFormatter::format_apple3_sos(const platform::LocalTime& lt)
{
    // Apple III SOS format (16 bytes): "YYYYMMDD0HHMMSS000"
    // Reference: SOS Reference Manual, page 94
    
    std::vector<std::uint8_t> result(16);
    
    // Format the string
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d0%02d%02d%02d000",
                  lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second);
    
    // Copy to result
    std::memcpy(result.data(), buffer, 16);
    
    return result;
}

std::string TimeFormatter::format_iso8601_tz(std::uint64_t unix_seconds, const std::string& timezone)
{
    char buffer[32];
    if (!platform::format_time_local_iso8601(unix_seconds, timezone.c_str(), buffer, sizeof(buffer))) {
        // Fallback to UTC if timezone fails
        return format_iso8601_utc(unix_seconds);
    }
    return std::string(buffer);
}

std::string TimeFormatter::format_iso8601_utc(std::uint64_t unix_seconds)
{
    char buffer[32];
    if (!platform::format_time_utc_iso8601(unix_seconds, buffer, sizeof(buffer))) {
        return "";
    }
    
    // Convert "YYYY-MM-DDTHH:MM:SSZ" to "YYYY-MM-DDTHH:MM:SS+0000"
    // The platform function returns Z format, we need +0000 for consistency
    std::string result(buffer);
    if (result.length() > 0 && result.back() == 'Z') {
        result.pop_back();
        result += "+0000";
    }
    
    return result;
}

} // namespace fujinet::time
