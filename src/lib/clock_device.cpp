#include "fujinet/io/devices/clock_device.h"

#include "fujinet/core/logging.h"
#include "fujinet/io/devices/file_codec.h"   // Reader + write_uXXle helpers
#include "fujinet/platform/time.h"

#include <string>
#include <cstdint>

namespace fujinet::io {

using fujinet::io::fileproto::Reader;
using fujinet::platform::set_unix_time_seconds;
using fujinet::platform::unix_time_seconds;

static const char* TAG = "clock";
static constexpr std::uint8_t CLOCKPROTO_VERSION = 1;

enum class ClockCommand : std::uint8_t {
    GetTime = 0x01,
    SetTime = 0x02,
};

// Clock response payload (v1):
// u8  version
// u8  flags (reserved, 0 for now)
// u16 reserved (LE, 0)
// u64 unix_seconds (LE)
static void build_time_payload(std::vector<std::uint8_t>& dst, std::uint64_t unixSeconds)
{
    std::string out;
    out.reserve(1 + 1 + 2 + 8);

    out.push_back(static_cast<char>(CLOCKPROTO_VERSION));
    out.push_back(static_cast<char>(0));         // flags
    fileproto::write_u16le(out, 0);              // reserved
    fileproto::write_u64le(out, unixSeconds);    // time

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
            if (!r.read_u8(ver) || ver != CLOCKPROTO_VERSION || !r.read_u64le(ts) || r.remaining() != 0) {
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

        default:
            return make_base_response(request, StatusCode::Unsupported);
    }
}

} // namespace fujinet::io
