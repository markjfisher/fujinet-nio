#include "doctest.h"

// Framing-through-core serial vs native parity lives in test_file_clock_core_parity.cpp.
#include "fujinet/io/devices/clock_commands.h"
#include "fujinet/io/devices/clock_device.h"

#include "fake_fs.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/io/core/io_device_manager.h"
#include "fujinet/platform/time.h"

#include <stdexcept>
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

namespace {

IORequest make_save_timezone_request()
{
    IORequest request{};
    request.id = 17;
    request.deviceId = 0x45;
    request.command = static_cast<std::uint16_t>(ClockCommand::SetTimezoneSave);
    request.payload = {1, 3, 'U', 'T', 'C'};
    return request;
}

class ThrowingClockStore final : public fujinet::config::FujiConfigStore {
public:
    fujinet::config::FujiConfig load() override
    {
        ++loads;
        if (failLoad) throw std::runtime_error("config read failed");
        return saved;
    }
    void save(const fujinet::config::FujiConfig& config) override
    {
        ++saves;
        if (failSave) throw std::runtime_error("config write failed");
        saved = config;
    }
    fujinet::config::FujiConfig saved;
    bool failLoad{false}, failSave{false};
    unsigned loads{0}, saves{0};
};

} // namespace

TEST_CASE("ClockDevice SetTimezoneSave ignores and preserves invalid Visor YAML")
{
    fujinet::platform::ScopedTimezone timezone("UTC0");
    fujinet::tests::MemoryFileSystem fs("primary");
    fujinet::config::YamlFujiConfigStoreFs store(&fs, nullptr, "fujinet.yaml");
    fujinet::config::FujiConfig initial;
    initial.clock.timezone = "UTC0";
    store.save(initial);
    fujinet::io::IODeviceManager devices;
    REQUIRE(devices.registerDevice(0x45, std::make_unique<ClockDevice>(&store)));
    const std::string invalid = "visor: {port: 3}\n";
    REQUIRE(fs.create_file("/fujinet.yaml", {invalid.begin(), invalid.end()}));
    auto request = make_save_timezone_request();
    fujinet::io::IOResponse response;
    REQUIRE_NOTHROW(response = devices.handleRequest(request));
    CHECK(response.status == StatusCode::Ok);
    CHECK(response.id == request.id);
    CHECK(response.deviceId == request.deviceId);
    CHECK(fujinet::platform::get_timezone() == "UTC");
    auto saved = store.load();
    CHECK(saved.clock.timezone == "UTC");
    CHECK_THROWS_AS(saved.visor.validate(), fujinet::config::InvalidVisorConfig);
    request.command = static_cast<std::uint16_t>(ClockCommand::GetTimezone);
    request.payload.clear();
    CHECK(devices.handleRequest(request).status == StatusCode::Ok);
}

TEST_CASE("ClockDevice SetTimezoneSave returns IOError for throwing config stores")
{
    fujinet::platform::ScopedTimezone timezone("UTC0");
    ThrowingClockStore store;
    store.saved.clock.timezone = "UTC0";
    SUBCASE("reload failure") { store.failLoad = true; }
    SUBCASE("save failure") { store.failSave = true; }
    ClockDevice device(&store);
    fujinet::io::IOResponse response;
    REQUIRE_NOTHROW(response = device.handle(make_save_timezone_request()));
    CHECK(response.status == StatusCode::IOError);
    CHECK(response.payload.empty());
    CHECK(store.saved.clock.timezone == "UTC0");
    CHECK(store.loads == 1);
    if (store.failLoad) {
        CHECK(store.saves == 0);
        CHECK(fujinet::platform::get_timezone() == "UTC0");
    } else CHECK(store.saves == 1);

    store.failLoad = store.failSave = false;
    REQUIRE_NOTHROW(response = device.handle(make_save_timezone_request()));
    CHECK(response.status == StatusCode::Ok);
    CHECK(store.saved.clock.timezone == "UTC");
}

TEST_CASE("ClockDevice SetTimezoneSave retains behavior without a config store")
{
    fujinet::platform::ScopedTimezone timezone("UTC0");
    ClockDevice device;
    const auto response = device.handle(make_save_timezone_request());
    CHECK(response.status == StatusCode::Ok);
    CHECK(fujinet::platform::get_timezone() == "UTC");
    CHECK(response.payload == std::vector<std::uint8_t>{1, 3, 'U', 'T', 'C', 0});
}
