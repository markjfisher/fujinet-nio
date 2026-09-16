#include "doctest.h"

#include "native_serial_core_parity.h"

#include "fujinet/io/devices/clock_commands.h"
#include "fujinet/io/devices/file_commands.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

using file_clock_core_parity::CoreParityStack;
using file_clock_core_parity::Framing;
using file_clock_core_parity::FrozenUnixTime;
using file_clock_core_parity::parse_list_entries;
using file_clock_core_parity::make_get_time_format_utc;
using file_clock_core_parity::make_list_request;
using file_clock_core_parity::make_set_time_payload;
using file_clock_core_parity::read_u64le;
using fujinet::io::ClockCommand;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FileCommand;
using fujinet::io::protocol::WireDeviceId;

namespace {

constexpr std::uint64_t kFrozenUnix = 1700000000ULL;
constexpr const char* kExpectedUtcIso = "2023-11-14T22:13:20+0000";

struct Pair {
    CoreParityStack serial{Framing::Serial, "host-serial"};
    CoreParityStack native{Framing::Native, "host-native"};
};

void require_replies_match(const file_clock_core_parity::DecodedReply& a,
                           const file_clock_core_parity::DecodedReply& b)
{
    REQUIRE(a.got);
    REQUIRE(b.got);
    CHECK(a.status == b.status);
    CHECK(a.device == b.device);
    CHECK(a.command == b.command);
    CHECK(a.payload == b.payload);
}

} // namespace

TEST_SUITE("file_clock_core_parity") {

TEST_CASE("serial and native ListDirectory replies match isolated fixture entries")
{
    Pair stacks;
    REQUIRE(stacks.serial.ready());
    REQUIRE(stacks.native.ready());
    const auto serial = stacks.serial.exchange(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        make_list_request(stacks.serial.root_uri()));
    const auto native = stacks.native.exchange(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        make_list_request(stacks.native.root_uri()));

    require_replies_match(serial, native);
    CHECK(serial.status == StatusCode::Ok);

    auto entries = parse_list_entries(serial.payload);
    REQUIRE(entries.size() == 2);
    const auto beta = std::find_if(entries.begin(), entries.end(),
                                   [](const auto& e) { return e.name == "beta"; });
    const auto alpha = std::find_if(entries.begin(), entries.end(),
                                    [](const auto& e) { return e.name == "alpha.txt"; });
    REQUIRE(beta != entries.end());
    REQUIRE(alpha != entries.end());
    CHECK((beta->flags & 0x01) != 0);
    CHECK((alpha->flags & 0x01) == 0);
    CHECK(alpha->size_bytes == 2);
}

TEST_CASE("serial and native GetTime and UTC GetTimeFormat match frozen unix time")
{
    FrozenUnixTime frozen(kFrozenUnix);
    Pair stacks;

    const auto serial_get = stacks.serial.exchange(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    const auto native_get = stacks.native.exchange(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    require_replies_match(serial_get, native_get);
    CHECK(serial_get.status == StatusCode::Ok);
    REQUIRE(serial_get.payload.size() >= 12);
    CHECK(read_u64le(serial_get.payload, 4) == kFrozenUnix);

    const auto serial_fmt = stacks.serial.exchange(
        WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::GetTimeFormat),
        make_get_time_format_utc());
    const auto native_fmt = stacks.native.exchange(
        WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::GetTimeFormat),
        make_get_time_format_utc());
    require_replies_match(serial_fmt, native_fmt);
    CHECK(serial_fmt.status == StatusCode::Ok);
    REQUIRE(serial_fmt.payload.size() >= 2);
    const std::string iso(serial_fmt.payload.begin() + 2, serial_fmt.payload.end());
    CHECK(iso == kExpectedUtcIso);
}

TEST_CASE("missing directory path retains FileDevice IOError on both framings")
{
    Pair stacks;
    const auto serial = stacks.serial.exchange(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        make_list_request(stacks.serial.fs_name() + ":/no-such-dir"));
    const auto native = stacks.native.exchange(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        make_list_request(stacks.native.fs_name() + ":/no-such-dir"));
    require_replies_match(serial, native);
    CHECK(serial.status == StatusCode::IOError);
}

TEST_CASE("unknown filesystem name retains DeviceNotFound on both framings")
{
    Pair stacks;
    const auto serial = stacks.serial.exchange(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        make_list_request("missingfs:/"));
    const auto native = stacks.native.exchange(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        make_list_request("missingfs:/"));
    require_replies_match(serial, native);
    CHECK(serial.status == StatusCode::DeviceNotFound);
}

TEST_CASE("truncated ListDirectory payload retains InvalidRequest on both framings")
{
    Pair stacks;
    const std::vector<std::uint8_t> truncated{1};
    const auto serial = stacks.serial.exchange(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        truncated);
    const auto native = stacks.native.exchange(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        truncated);
    require_replies_match(serial, native);
    CHECK(serial.status == StatusCode::InvalidRequest);
}

TEST_CASE("malformed GetTimeFormat payload retains InvalidRequest on both framings")
{
    Pair stacks;
    const std::vector<std::uint8_t> truncated{1, 4, 3, 'U'};
    const auto serial = stacks.serial.exchange(
        WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::GetTimeFormat),
        truncated);
    const auto native = stacks.native.exchange(
        WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::GetTimeFormat),
        truncated);
    require_replies_match(serial, native);
    CHECK(serial.status == StatusCode::InvalidRequest);

    const std::vector<std::uint8_t> trailing{1, 4, 3, 'U', 'T', 'C', 0xAA};
    const auto serial_tr = stacks.serial.exchange(
        WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::GetTimeFormat),
        trailing);
    const auto native_tr = stacks.native.exchange(
        WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::GetTimeFormat),
        trailing);
    require_replies_match(serial_tr, native_tr);
    CHECK(serial_tr.status == StatusCode::InvalidRequest);
}

TEST_CASE("unavailable unix time retains GetTime NotReady on both framings")
{
    FrozenUnixTime frozen(0);
    Pair stacks;
    const auto serial = stacks.serial.exchange(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    const auto native = stacks.native.exchange(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    require_replies_match(serial, native);
    CHECK(serial.status == StatusCode::NotReady);
}

TEST_CASE("POSIX SetTime retains IOError and does not change the host clock")
{
    Pair stacks;
    const std::uint64_t attempted = 12345;
    const auto before = static_cast<std::uint64_t>(std::time(nullptr));
    const auto serial = stacks.serial.exchange(
        WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::SetTime),
        make_set_time_payload(attempted));
    const auto native = stacks.native.exchange(
        WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::SetTime),
        make_set_time_payload(attempted));
    const auto after = static_cast<std::uint64_t>(std::time(nullptr));

    require_replies_match(serial, native);
    CHECK(serial.status == StatusCode::IOError);
    CHECK(before != attempted);
    CHECK(after != attempted);
    CHECK(after >= before);
    CHECK(after - before <= 2);
}

} // TEST_SUITE
