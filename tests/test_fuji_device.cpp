#include "doctest.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/devices/fuji_commands.h"
#include "fujinet/io/devices/fuji_device.h"

#include <memory>

namespace {

using fujinet::config::FujiConfig;
using fujinet::config::FujiConfigStore;
using fujinet::io::FujiDevice;
using fujinet::io::IORequest;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FujiCommand;

class MemoryFujiConfigStore final : public FujiConfigStore {
public:
    explicit MemoryFujiConfigStore(FujiConfig initial)
        : config(std::move(initial))
    {
    }

    FujiConfig load() override
    {
        ++loadCount;
        return config;
    }

    void save(const FujiConfig& cfg) override
    {
        config = cfg;
        ++saveCount;
    }

    FujiConfig config;
    int loadCount{0};
    int saveCount{0};
};

} // namespace

TEST_CASE("FujiDevice loads non-mount configuration on start")
{
    FujiConfig initial;
    initial.general.deviceName = "test-fujinet";
    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    auto* storePtr = store.get();
    FujiDevice device(nullptr, std::move(store));

    device.start();

    CHECK(storePtr->loadCount == 1);
    CHECK(device.config().general.deviceName == "test-fujinet");
}

TEST_CASE("FujiDevice reset invokes the platform reset handler")
{
    bool resetCalled = false;
    auto store = std::make_unique<MemoryFujiConfigStore>(FujiConfig{});
    FujiDevice device([&resetCalled] { resetCalled = true; }, std::move(store));

    IORequest request{};
    request.command = static_cast<std::uint16_t>(FujiCommand::Reset);
    const auto response = device.handle(request);

    CHECK(response.status == StatusCode::Ok);
    CHECK(resetCalled);
}

TEST_CASE("FujiDevice rejects retired mount configuration commands")
{
    auto store = std::make_unique<MemoryFujiConfigStore>(FujiConfig{});
    FujiDevice device(nullptr, std::move(store));

    for (const std::uint16_t command : {0xFDU, 0xFCU, 0xFBU}) {
        IORequest request{};
        request.command = command;
        CHECK(device.handle(request).status == StatusCode::Unsupported);
    }
}
