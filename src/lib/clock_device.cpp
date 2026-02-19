#include "fujinet/io/devices/clock_device.h"

#include "fujinet/core/logging.h"
#include "fujinet/io/devices/clock_commands.h"
#include "fujinet/io/devices/file_codec.h"
#include "fujinet/platform/time.h"
#include "fujinet/time/time_formatter.h"

#include <string>
#include <cstdint>
#include <stdexcept>

namespace fujinet::io {

using fujinet::io::fileproto::Reader;
using fujinet::io::fileproto::write_u16le;
using fujinet::io::fileproto::write_u64le;
using fujinet::platform::set_unix_time_seconds;
using fujinet::platform::unix_time_seconds;
using fujinet::platform::set_timezone;
using fujinet::platform::get_timezone;
using fujinet::platform::validate_timezone;

static const char* TAG = "clock";

// Build time payload for GetTime/SetTime responses
// u8  version
// u8  flags (reserved, 0 for now)
// u16 reserved (LE, 0)
// u64 unix_seconds (LE)
static void build_time_payload(std::vector<std::uint8_t>& dst, std::uint64_t unixSeconds)
{
    std::string out;
    out.reserve(1 + 1 + 2 + 8);

    out.push_back(static_cast<char>(CLOCK_PROTO_VERSION));
    out.push_back(static_cast<char>(0));         // flags
    write_u16le(out, 0);                         // reserved
    write_u64le(out, unixSeconds);               // time

    dst.assign(out.begin(), out.end());
}

// Build formatted time response
// u8  version
// u8  format (echo)
// u8[] formatted_time
static void build_formatted_payload(std::vector<std::uint8_t>& dst, 
                                     std::uint8_t format,
                                     const std::vector<std::uint8_t>& formatted_time)
{
    std::string out;
    out.reserve(1 + 1 + formatted_time.size());

    out.push_back(static_cast<char>(CLOCK_PROTO_VERSION));
    out.push_back(static_cast<char>(format));
    out.append(formatted_time.begin(), formatted_time.end());

    dst.assign(out.begin(), out.end());
}

// Build timezone response
// u8  version
// u8  length
// char[] timezone_string (null-terminated)
static void build_timezone_payload(std::vector<std::uint8_t>& dst, const std::string& tz)
{
    std::string out;
    out.reserve(1 + 1 + tz.size() + 1);

    out.push_back(static_cast<char>(CLOCK_PROTO_VERSION));
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(tz.size())));
    out.append(tz);
    out.push_back('\0');  // null terminator

    dst.assign(out.begin(), out.end());
}

IOResponse ClockDevice::handle(const IORequest& request)
{
    const auto cmd = static_cast<ClockCommand>(request.command & 0xFF);

    switch (cmd) {
        case ClockCommand::GetTime: {
            auto resp = make_success_response(request);

            const std::uint64_t now = unix_time_seconds();
            if (now == 0) {
                resp.status = StatusCode::NotReady;
                return resp;
            }

            build_time_payload(resp.payload, now);
            return resp;
        }

        case ClockCommand::SetTime: {
            auto resp = make_success_response(request);

            Reader r(request.payload.data(), request.payload.size());
            std::uint8_t ver = 0;
            std::uint64_t ts = 0;

            // SetTime request payload (v1):
            // u8  version
            // u64 unix_seconds (LE)
            if (!r.read_u8(ver) || ver != CLOCK_PROTO_VERSION || !r.read_u64le(ts) || r.remaining() != 0) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            if (!set_unix_time_seconds(ts)) {
                resp.status = StatusCode::IOError;
                return resp;
            }

            // Echo back what we set: same payload shape as GetTime.
            build_time_payload(resp.payload, ts);

            FN_LOGI(TAG, "Time set to %llu", static_cast<unsigned long long>(ts));
            return resp;
        }

        case ClockCommand::GetTimeFormat: {
            auto resp = make_success_response(request);

            Reader r(request.payload.data(), request.payload.size());
            std::uint8_t ver = 0;
            std::uint8_t format = 0;

            // GetTimeFormat request payload:
            // u8  version
            // u8  format (TimeFormat enum)
            // optional: u8 tz_len + char[] timezone (for non-UTC formats)
            if (!r.read_u8(ver) || ver != CLOCK_PROTO_VERSION || !r.read_u8(format)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            // Get timezone from request or use current system timezone
            std::string tz = "UTC";
            std::uint8_t tz_len = 0;
            if (r.read_u8(tz_len) && tz_len > 0) {
                std::string_view tz_view;
                if (r.read_sv(tz_view, tz_len)) {
                    tz = std::string(tz_view);
                }
            } else {
                // Use current system timezone
                tz = get_timezone();
            }

            // Validate timezone
            if (!validate_timezone(tz.c_str())) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            // Get current time
            const std::uint64_t now = unix_time_seconds();
            if (now == 0) {
                resp.status = StatusCode::NotReady;
                return resp;
            }

            // Format the time
            try {
                auto formatted = time::TimeFormatter::format_time(
                    now, 
                    static_cast<TimeFormat>(format), 
                    tz
                );
                build_formatted_payload(resp.payload, format, formatted);
            } catch (const std::invalid_argument& e) {
                FN_LOGW(TAG, "Time format error: %s", e.what());
                resp.status = StatusCode::InvalidRequest;
            }

            return resp;
        }

        case ClockCommand::GetTimezone: {
            auto resp = make_success_response(request);

            std::string tz = get_timezone();
            build_timezone_payload(resp.payload, tz);

            return resp;
        }

        case ClockCommand::SetTimezone: {
            auto resp = make_success_response(request);

            Reader r(request.payload.data(), request.payload.size());
            std::uint8_t ver = 0;
            std::uint8_t tz_len = 0;

            // SetTimezone request payload:
            // u8  version
            // u8  length
            // char[] timezone_string
            if (!r.read_u8(ver) || ver != CLOCK_PROTO_VERSION || !r.read_u8(tz_len)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            if (tz_len == 0 || tz_len > MAX_TIMEZONE_LEN) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::string_view tz_view;
            if (!r.read_sv(tz_view, tz_len)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            std::string tz(tz_view);

            // Validate timezone
            if (!validate_timezone(tz.c_str())) {
                FN_LOGW(TAG, "Invalid timezone: %s", tz.c_str());
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            // Set the timezone (non-persistent)
            if (!set_timezone(tz.c_str())) {
                resp.status = StatusCode::IOError;
                return resp;
            }

            FN_LOGI(TAG, "Timezone set to: %s", tz.c_str());
            build_timezone_payload(resp.payload, tz);

            return resp;
        }

        case ClockCommand::SetTimezoneSave: {
            auto resp = make_success_response(request);

            Reader r(request.payload.data(), request.payload.size());
            std::uint8_t ver = 0;
            std::uint8_t tz_len = 0;

            // SetTimezoneSave request payload (same as SetTimezone):
            // u8  version
            // u8  length
            // char[] timezone_string
            if (!r.read_u8(ver) || ver != CLOCK_PROTO_VERSION || !r.read_u8(tz_len)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            if (tz_len == 0 || tz_len > MAX_TIMEZONE_LEN) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            std::string_view tz_view;
            if (!r.read_sv(tz_view, tz_len)) {
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }
            std::string tz(tz_view);

            // Validate timezone
            if (!validate_timezone(tz.c_str())) {
                FN_LOGW(TAG, "Invalid timezone: %s", tz.c_str());
                resp.status = StatusCode::InvalidRequest;
                return resp;
            }

            // Set the timezone
            if (!set_timezone(tz.c_str())) {
                resp.status = StatusCode::IOError;
                return resp;
            }

            // TODO: Persist to config
            // This would require access to the config store, which should be
            // injected into the ClockDevice. For now, just log the intent.
            FN_LOGI(TAG, "Timezone set and saved: %s", tz.c_str());
            
            build_timezone_payload(resp.payload, tz);

            return resp;
        }

        default:
            return make_base_response(request, StatusCode::Unsupported);
    }
}

} // namespace fujinet::io
