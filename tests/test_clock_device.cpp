#include "doctest.h"

#include "fujinet/io/devices/clock_commands.h"
#include "fujinet/io/devices/clock_device.h"

#include <string>

using fujinet::io::ClockDevice;
using fujinet::io::IORequest;
using fujinet::io::StatusCode;
using fujinet::io::ClockCommand;

namespace {

IORequest make_get_time_format_request(const std::vector<std::uint8_t>& payload)
{
    IORequest req{};
    req.id = 1;
    req.deviceId = 0x45;
    req.command = static_cast<std::uint16_t>(ClockCommand::GetTimeFormat);
    req.payload = payload;
    return req;
}

} // namespace

TEST_CASE("ClockDevice GetTimeFormat rejects truncated timezone payload")
{
    ClockDevice device;

    auto resp = device.handle(make_get_time_format_request({1, 4, 3, 'U'}));

    CHECK(resp.status == StatusCode::InvalidRequest);
}

TEST_CASE("ClockDevice GetTimeFormat accepts a valid UTC ISO request")
{
    ClockDevice device;

    auto resp = device.handle(make_get_time_format_request(
        {1, static_cast<std::uint8_t>(fujinet::io::TimeFormat::UtcIsoString)}));

    REQUIRE(resp.status == StatusCode::Ok);
    REQUIRE(resp.payload.size() >= 2);
    CHECK(resp.payload[0] == 1);
    CHECK(resp.payload[1] == static_cast<std::uint8_t>(fujinet::io::TimeFormat::UtcIsoString));

    const std::string formatted(resp.payload.begin() + 2, resp.payload.end());
    CHECK(!formatted.empty());
    CHECK(formatted.size() == 24);
    CHECK(formatted.find('T') != std::string::npos);
    CHECK(formatted.substr(formatted.size() - 5) == "+0000");
}

TEST_CASE("ClockDevice GetTimeFormat accepts a valid timezone request")
{
    ClockDevice device;

    auto resp = device.handle(make_get_time_format_request(
        {1, static_cast<std::uint8_t>(fujinet::io::TimeFormat::TzIsoString), 3, 'U', 'T', 'C'}));

    REQUIRE(resp.status == StatusCode::Ok);
    REQUIRE(resp.payload.size() >= 2);
    CHECK(resp.payload[0] == 1);
    CHECK(resp.payload[1] == static_cast<std::uint8_t>(fujinet::io::TimeFormat::TzIsoString));

    const std::string formatted(resp.payload.begin() + 2, resp.payload.end());
    CHECK(!formatted.empty());
    CHECK(formatted.size() == 24);
    CHECK(formatted.find('T') != std::string::npos);
    CHECK(formatted.substr(formatted.size() - 5) == "+0000");
}

TEST_CASE("ClockDevice GetTimeFormat rejects overlong timezone length")
{
    ClockDevice device;

    auto resp = device.handle(make_get_time_format_request({1, 4, 65}));

    CHECK(resp.status == StatusCode::InvalidRequest);
}

TEST_CASE("ClockDevice GetTimeFormat rejects trailing bytes after timezone")
{
    ClockDevice device;

    auto resp = device.handle(make_get_time_format_request({1, 4, 3, 'U', 'T', 'C', 0xAA}));

    CHECK(resp.status == StatusCode::InvalidRequest);
}
