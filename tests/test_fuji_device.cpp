#include "doctest.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/fuji_commands.h"
#include "fujinet/io/devices/fuji_device.h"

#include <memory>

namespace {

constexpr std::uint8_t kGetMountsFlagFormatted = 0x01U;
constexpr std::uint8_t kGetMountsRespFlagMore = 0x01U;
constexpr std::uint8_t kGetMountsRespFlagFormatted = 0x02U;

std::vector<std::uint8_t> get_mounts_payload(
    std::uint8_t flags,
    std::uint16_t firstSlot,
    std::uint16_t lastSlot,
    std::uint16_t startIndex,
    std::uint16_t maxPayloadBytes)
{
    return {
        flags,
        static_cast<std::uint8_t>(firstSlot & 0xFF),
        static_cast<std::uint8_t>((firstSlot >> 8) & 0xFF),
        static_cast<std::uint8_t>(lastSlot & 0xFF),
        static_cast<std::uint8_t>((lastSlot >> 8) & 0xFF),
        static_cast<std::uint8_t>(startIndex & 0xFF),
        static_cast<std::uint8_t>((startIndex >> 8) & 0xFF),
        static_cast<std::uint8_t>(maxPayloadBytes & 0xFF),
        static_cast<std::uint8_t>((maxPayloadBytes >> 8) & 0xFF),
    };
}

std::uint16_t read_u16le(const std::vector<std::uint8_t>& payload, std::size_t offset)
{
    return static_cast<std::uint16_t>(payload[offset] | (static_cast<std::uint16_t>(payload[offset + 1]) << 8));
}

using fujinet::config::FujiConfig;
using fujinet::config::FujiConfigStore;
using fujinet::config::MountConfig;
using fujinet::io::FujiDevice;
using fujinet::io::IORequest;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FujiCommand;

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

TEST_CASE("FujiDevice returns extended binary mount table with slot range filtering")
{
    FujiConfig initial;
    initial.mounts.push_back(MountConfig{2, "tnfs://server/game1.atr", "r", true});
    initial.mounts.push_back(MountConfig{11, "host:/images/game2.atr", "rw", false});
    initial.mounts.push_back(MountConfig{300, "sd:/archive/game3.atr", "r", true});

    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::GetMounts);
    req.payload = get_mounts_payload(0x00, 10, 300, 0, 512);

    auto resp = device.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    REQUIRE(resp.payload.size() > 10);
    CHECK(resp.payload[0] == 0x01);
    CHECK(resp.payload[1] == 0x00);
    CHECK(read_u16le(resp.payload, 2) == 10);
    CHECK(read_u16le(resp.payload, 4) == 0);
    CHECK(read_u16le(resp.payload, 6) == 2);

    std::size_t offset = 10;
    CHECK(read_u16le(resp.payload, offset) == 11);
    offset += 2;
    CHECK(resp.payload[offset++] == 0x00);
    const auto firstUriLen = resp.payload[offset++];
    CHECK(std::string(resp.payload.begin() + offset, resp.payload.begin() + offset + firstUriLen) == "host:/images/game2.atr");
    offset += firstUriLen;
    const auto firstModeLen = resp.payload[offset++];
    CHECK(std::string(resp.payload.begin() + offset, resp.payload.begin() + offset + firstModeLen) == "rw");
    offset += firstModeLen;

    CHECK(read_u16le(resp.payload, offset) == 300);
    offset += 2;
    CHECK(resp.payload[offset++] == 0x01);
    const auto secondUriLen = resp.payload[offset++];
    CHECK(std::string(resp.payload.begin() + offset, resp.payload.begin() + offset + secondUriLen) == "sd:/archive/game3.atr");
}

TEST_CASE("FujiDevice returns formatted mount lines for extended requests")
{
    FujiConfig initial;
    initial.mounts.push_back(MountConfig{2, "tnfs://server/game1.atr", "r", true});
    initial.mounts.push_back(MountConfig{11, "host:/images/game2.atr", "rw", false});

    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::GetMounts);
    req.payload = get_mounts_payload(kGetMountsFlagFormatted, 0, 0, 0, 512);

    auto resp = device.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    REQUIRE(resp.payload.size() >= 10);
    CHECK(resp.payload[0] == 0x01);
    CHECK(resp.payload[1] == kGetMountsRespFlagFormatted);
    CHECK(read_u16le(resp.payload, 2) == 2);
    CHECK(read_u16le(resp.payload, 4) == 0);
    CHECK(read_u16le(resp.payload, 6) == 2);

    const auto entriesLen = read_u16le(resp.payload, 8);
    CHECK(entriesLen == resp.payload.size() - 10);
    const std::string text(resp.payload.begin() + 10, resp.payload.end());
    CHECK(text == "2:* [R] tnfs://server\n  path: /game1.atr\n11:  [RW] host:\n  path: /images/game2.atr\n");
}

TEST_CASE("FujiDevice paginates formatted mount lines when max payload is small")
{
    FujiConfig initial;
    initial.mounts.push_back(MountConfig{2, "tnfs://server/game1.atr", "r", true});
    initial.mounts.push_back(MountConfig{11, "host:/images/game2.atr", "rw", false});

    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::GetMounts);
    req.payload = get_mounts_payload(kGetMountsFlagFormatted, 0, 0, 0, 48);

    auto first = device.handle(req);
    REQUIRE(first.status == StatusCode::Ok);
    CHECK((first.payload[1] & kGetMountsRespFlagFormatted) != 0);
    CHECK((first.payload[1] & kGetMountsRespFlagMore) != 0);
    CHECK(read_u16le(first.payload, 4) == 0);
    CHECK(read_u16le(first.payload, 6) == 1);
    CHECK(read_u16le(first.payload, 8) > 0);

    req.payload = get_mounts_payload(kGetMountsFlagFormatted, 0, 0, 1, 48);
    auto second = device.handle(req);
    REQUIRE(second.status == StatusCode::Ok);
    CHECK((second.payload[1] & kGetMountsRespFlagFormatted) != 0);
    CHECK((second.payload[1] & kGetMountsRespFlagMore) == 0);
    CHECK(read_u16le(second.payload, 4) == 1);
    CHECK(read_u16le(second.payload, 6) == 1);
    CHECK(read_u16le(second.payload, 8) > 0);
}

TEST_CASE("FujiDevice defaults extended mount range to configured bounds")
{
    FujiConfig initial;
    initial.mounts.push_back(MountConfig{25, "tnfs://server/game1.atr", "r", true});
    initial.mounts.push_back(MountConfig{1200, "host:/images/game2.atr", "rw", true});

    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::GetMounts);
    req.payload = get_mounts_payload(0x00, 0, 0, 0, 512);

    auto resp = device.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    CHECK(read_u16le(resp.payload, 2) == 25);
    CHECK(read_u16le(resp.payload, 4) == 0);
    CHECK(read_u16le(resp.payload, 6) == 2);
}

TEST_CASE("FujiDevice rejects malformed extended get mounts requests")
{
    auto store = std::make_unique<MemoryFujiConfigStore>(FujiConfig{});
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::GetMounts);
    req.payload = {0x00, 0x01, 0x00};

    auto resp = device.handle(req);
    CHECK(resp.status == StatusCode::InvalidRequest);
}

TEST_CASE("FujiDevice rejects extended get mounts requests with zero max payload")
{
    auto store = std::make_unique<MemoryFujiConfigStore>(FujiConfig{});
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::GetMounts);
    req.payload = get_mounts_payload(kGetMountsFlagFormatted, 0, 0, 0, 0);

    auto resp = device.handle(req);
    CHECK(resp.status == StatusCode::InvalidRequest);
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

TEST_CASE("FujiDevice accepts a 4-byte clear mount request with empty URI and mode")
{
    FujiConfig initial;
    initial.mounts.push_back(MountConfig{2, "tnfs://server/game.atr", "rw", true});

    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    auto* storePtr = store.get();
    fujinet::fs::StorageManager storage;
    FujiDevice device(nullptr, std::move(store), storage);
    device.start();

    IORequest req{};
    req.id = 1;
    req.deviceId = 0x70;
    req.command = static_cast<std::uint16_t>(FujiCommand::SetMount);
    req.payload = {1, 0x00, 0x00, 0x00};

    auto resp = device.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    CHECK(storePtr->_config.mounts.empty());
    CHECK(storePtr->saveCount == 1);
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
