#include "doctest.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/fuji_commands.h"
#include "fujinet/io/devices/fuji_device.h"

#include <memory>

using fujinet::config::FujiConfig;
using fujinet::config::FujiConfigStore;
using fujinet::config::MountConfig;
using fujinet::io::FujiDevice;
using fujinet::io::IORequest;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FujiCommand;

namespace {

class MemoryFujiConfigStore final : public FujiConfigStore {
public:
    explicit MemoryFujiConfigStore(FujiConfig initial)
        : _config(std::move(initial))
    {
    }

    FujiConfig load() override
    {
        return _config;
    }

    void save(const FujiConfig& cfg) override
    {
        _config = cfg;
        ++saveCount;
    }

    FujiConfig _config;
    int saveCount{0};
};

std::vector<std::uint8_t> set_mount_payload(std::uint8_t slotIndex,
                                            std::uint8_t flags,
                                            const std::string& uri,
                                            const std::string& mode)
{
    std::vector<std::uint8_t> payload;
    payload.push_back(slotIndex);
    payload.push_back(flags);
    payload.push_back(static_cast<std::uint8_t>(uri.size()));
    payload.insert(payload.end(), uri.begin(), uri.end());
    payload.push_back(static_cast<std::uint8_t>(mode.size()));
    payload.insert(payload.end(), mode.begin(), mode.end());
    return payload;
}

} // namespace

TEST_CASE("FujiDevice persists mount updates using 0-based slot indices")
{
    FujiConfig initial;
    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    auto* storePtr = store.get();

    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest setReq{};
    setReq.id = 1;
    setReq.deviceId = 0x70;
    setReq.command = static_cast<std::uint16_t>(FujiCommand::SetMount);
    setReq.payload = set_mount_payload(0, 0x01, "sd:/disks/boot.atr", "rw");

    auto setResp = device.handle(setReq);
    REQUIRE(setResp.status == StatusCode::Ok);
    REQUIRE(storePtr->saveCount == 1);
    REQUIRE(storePtr->_config.mounts.size() == 1);
    CHECK(storePtr->_config.mounts[0].slot == 1);
    CHECK(storePtr->_config.mounts[0].uri == "sd:/disks/boot.atr");
    CHECK(storePtr->_config.mounts[0].mode == "rw");
    CHECK(storePtr->_config.mounts[0].enabled == true);

    IORequest getReq{};
    getReq.id = 2;
    getReq.deviceId = 0x70;
    getReq.command = static_cast<std::uint16_t>(FujiCommand::GetMount);
    getReq.payload = {0};

    auto getResp = device.handle(getReq);
    REQUIRE(getResp.status == StatusCode::Ok);
    REQUIRE(getResp.payload.size() == 1 + 1 + 1 + 18 + 1 + 2);
    CHECK(getResp.payload[0] == 0);
    CHECK(getResp.payload[1] == 0x01);
    CHECK(getResp.payload[2] == 18);
    CHECK(std::string(getResp.payload.begin() + 3, getResp.payload.begin() + 21) == "sd:/disks/boot.atr");
    CHECK(getResp.payload[21] == 2);
    CHECK(std::string(getResp.payload.begin() + 22, getResp.payload.end()) == "rw");
}

TEST_CASE("FujiDevice returns sorted persisted mount table with 0-based indices")
{
    FujiConfig initial;
    initial.mounts.push_back(MountConfig{3, "host:/images/data.atr", "rw", true});
    initial.mounts.push_back(MountConfig{1, "sd:/disks/boot.atr", "r", true});
    initial.mounts.push_back(MountConfig{0, "ignored:/invalid", "r", true});

    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::GetMounts);

    auto resp = device.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    REQUIRE(!resp.payload.empty());
    CHECK(resp.payload[0] == 2);
    CHECK(resp.payload[1] == 0);
    CHECK(resp.payload[2] == 0x01);

    const auto firstUriLen = resp.payload[3];
    CHECK(std::string(resp.payload.begin() + 4, resp.payload.begin() + 4 + firstUriLen) == "sd:/disks/boot.atr");

    const auto secondOffset = static_cast<std::size_t>(4 + firstUriLen + 1 + resp.payload[4 + firstUriLen]);
    REQUIRE(secondOffset < resp.payload.size());
    CHECK(resp.payload[secondOffset] == 2);
}

TEST_CASE("FujiDevice clears a persisted mount when URI is empty")
{
    FujiConfig initial;
    initial.mounts.push_back(MountConfig{2, "tnfs://server/game.atr", "r", true});

    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    auto* storePtr = store.get();
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::SetMount);
    req.payload = set_mount_payload(1, 0x00, "", "r");

    auto resp = device.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    CHECK(storePtr->_config.mounts.empty());
}

TEST_CASE("FujiDevice reports an empty persisted mount record for an unused slot")
{
    auto store = std::make_unique<MemoryFujiConfigStore>(FujiConfig{});
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::GetMount);
    req.payload = {7};

    auto resp = device.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    REQUIRE(resp.payload.size() == 5);
    CHECK(resp.payload[0] == 7);
    CHECK(resp.payload[1] == 0x00);
    CHECK(resp.payload[2] == 0);
    CHECK(resp.payload[3] == 1);
    CHECK(resp.payload[4] == 'r');
}
