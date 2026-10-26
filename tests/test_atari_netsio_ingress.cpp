#include "doctest.h"

#include "fujinet/core/core.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/atari_sio_nio_frontend.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/platform/posix/atari_netsio_fujibus_channel.h"

#include "../third_party/cjson/cJSON.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint8_t data_byte = 0x01;
constexpr std::uint8_t data_block = 0x02;
constexpr std::uint8_t data_byte_sync = 0x09;
constexpr std::uint8_t command_on = 0x11;
constexpr std::uint8_t command_off_sync = 0x18;
constexpr std::uint8_t sync_response = 0x81;
constexpr std::uint8_t empty_sync = 0x00;
constexpr std::uint8_t ack_sync = 0x01;

std::vector<std::uint8_t> hex_bytes(const char* text)
{
    std::vector<std::uint8_t> out;
    std::istringstream input(text ? text : "");
    unsigned value = 0;
    while (input >> std::hex >> value) out.push_back(static_cast<std::uint8_t>(value));
    return out;
}

struct Fixture {
    std::array<std::uint8_t, 5> command{};
    std::vector<std::uint8_t> payload;
};

Fixture fixture(const char* id)
{
    std::ifstream input(std::filesystem::path(FN_ATARI_SIO_FIXTURE_DIR) / "nio_v1.json");
    REQUIRE(input.good());
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    cJSON* root = cJSON_Parse(json.c_str());
    REQUIRE(root != nullptr);
    const cJSON* found = nullptr;
    cJSON_ArrayForEach(found, cJSON_GetObjectItemCaseSensitive(root, "scenarios")) {
        const auto* candidate = cJSON_GetObjectItemCaseSensitive(found, "id");
        if (cJSON_IsString(candidate) && std::string(candidate->valuestring) == id) break;
    }
    REQUIRE(found != nullptr);
    Fixture result{};
    const auto command = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "frame")));
    REQUIRE(command.size() == result.command.size());
    std::copy(command.begin(), command.end(), result.command.begin());
    result.payload = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "payload")));
    cJSON_Delete(root);
    return result;
}

std::array<std::uint8_t, 5> command(std::uint8_t device, std::uint8_t op, std::uint16_t aux)
{
    std::array<std::uint8_t, 5> result{device, op, static_cast<std::uint8_t>(aux),
                                       static_cast<std::uint8_t>(aux >> 8), 0};
    result.back() = fujinet::io::AtariSioNioFrontend::checksum(result.data(), result.size() - 1);
    return result;
}

std::vector<std::uint8_t> sio_reply(const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> result{'A', 'C'};
    result.insert(result.end(), payload.begin(), payload.end());
    result.push_back(fujinet::io::AtariSioNioFrontend::checksum(payload.data(), payload.size()));
    return result;
}

class DatagramChannel final : public fujinet::io::Channel {
public:
    void push(std::vector<std::uint8_t> datagram) { rx.push_back(std::move(datagram)); }
    void clear_tx() { tx.clear(); }
    bool available() override { return !rx.empty(); }
    std::size_t read(std::uint8_t* buffer, std::size_t max_len) override
    {
        if (rx.empty()) return 0;
        const auto datagram = std::move(rx.front());
        rx.pop_front();
        REQUIRE(datagram.size() <= max_len);
        std::memcpy(buffer, datagram.data(), datagram.size());
        return datagram.size();
    }
    void write(const std::uint8_t* buffer, std::size_t len) override { tx.emplace_back(buffer, buffer + len); }

    std::deque<std::vector<std::uint8_t>> rx;
    std::vector<std::vector<std::uint8_t>> tx;
};

class RecordingDevice final : public fujinet::io::VirtualDevice {
public:
    fujinet::io::IOResponse handle(const fujinet::io::IORequest& request) override
    {
        requests.push_back(request);
        auto response = fujinet::io::make_success_response(request);
        response.payload = {0xaa, 0x55};
        return response;
    }
    std::vector<fujinet::io::IORequest> requests;
};

struct Harness {
    fujinet::fs::StorageManager storage;
    fujinet::disk::DiskService disks{storage, fujinet::disk::make_default_image_registry()};
    fujinet::core::FujinetCore core;
    RecordingDevice* device{new RecordingDevice()};
    DatagramChannel* wire{new DatagramChannel()};
    std::unique_ptr<fujinet::platform::posix::AtariNetSioIngress> ingress;

    Harness()
    {
        REQUIRE(core.deviceManager().registerDevice(1, std::unique_ptr<fujinet::io::VirtualDevice>(device)));
        ingress = fujinet::platform::posix::create_atari_netsio_ingress(
            std::unique_ptr<fujinet::io::Channel>(wire), disks, core.routingManager());
        wire->clear_tx(); // construction lifecycle is tested by boot acceptance, not each fixture exchange.
    }

    void poll() { ingress->poll(); }
    void command_frame(const std::array<std::uint8_t, 5>& frame, std::uint8_t sync)
    {
        wire->push({command_on});
        std::vector<std::uint8_t> block{data_block};
        block.insert(block.end(), frame.begin(), frame.end());
        block.push_back(0); // NetSIO data-block sequence byte, excluded by its parser.
        wire->push(std::move(block));
        wire->push({command_off_sync, sync});
        poll();
    }
    void write_data(const std::vector<std::uint8_t>& payload, std::uint8_t sync)
    {
        std::vector<std::uint8_t> block{data_block};
        block.insert(block.end(), payload.begin(), payload.end());
        block.push_back(0);
        wire->push(std::move(block));
        wire->push({data_byte_sync,
                    fujinet::io::AtariSioNioFrontend::checksum(payload.data(), payload.size()), sync});
        poll();
    }
};

std::vector<std::uint8_t> decode_reply(const std::vector<std::vector<std::uint8_t>>& datagrams,
                                       std::uint8_t sync)
{
    std::vector<std::uint8_t> reply;
    for (const auto& datagram : datagrams) {
        if (datagram.size() == 6 && datagram[0] == sync_response && datagram[1] == sync) {
            REQUIRE(datagram[2] == ack_sync);
            reply.push_back(datagram[3]);
        } else if (datagram.size() >= 2 && datagram[0] == data_byte) {
            reply.push_back(datagram[1]);
        } else if (datagram.size() >= 2 && datagram[0] == data_block) {
            reply.insert(reply.end(), datagram.begin() + 1, datagram.end());
        }
    }
    return reply;
}

void require_write_ack(const Harness& harness, std::uint8_t sync, std::size_t bytes)
{
    REQUIRE(harness.wire->tx.size() >= 2);
    CHECK(harness.wire->tx[harness.wire->tx.size() - 1] ==
          std::vector<std::uint8_t>{sync_response, sync, ack_sync, 'A',
                                    static_cast<std::uint8_t>((bytes + 1) & 0xffU),
                                    static_cast<std::uint8_t>((bytes + 1) >> 8U)});
}

} // namespace

TEST_CASE("Atari NetSIO ingress routes canonical Q/W/P/R through the production endpoint")
{
    Harness harness;
    const auto query = fixture("q-idle");
    const auto write = fixture("w-core-dispatch");
    const auto poll = fixture("p-core-dispatch-ready");
    const auto read = fixture("r-core-dispatch");

    harness.command_frame(query.command, 1);
    CHECK(decode_reply(harness.wire->tx, 1) == sio_reply({1, 0, 2, 0}));
    harness.wire->clear_tx();

    harness.command_frame(write.command, 2);
    require_write_ack(harness, 2, write.payload.size());
    harness.wire->clear_tx();
    harness.write_data(write.payload, 3);
    CHECK(decode_reply(harness.wire->tx, 3) == std::vector<std::uint8_t>{'A', 'C'});
    REQUIRE(harness.device->requests.size() == 1);
    harness.wire->clear_tx();

    harness.command_frame(poll.command, 4);
    CHECK(decode_reply(harness.wire->tx, 4) == sio_reply({2, 9, 0}));
    harness.wire->clear_tx();
    harness.command_frame(read.command, 5);
    CHECK(decode_reply(harness.wire->tx, 5) == sio_reply(read.payload));
}

TEST_CASE("Atari NetSIO ingress rejects aliases, retains wrong R, and resets the production endpoint")
{
    Harness harness;
    const auto write = fixture("w-core-dispatch");
    const auto read = fixture("r-core-dispatch");
    const auto poll = fixture("p-core-dispatch-ready");

    harness.command_frame(command(0x71, 'Q', 0), 1);
    REQUIRE(harness.wire->tx.size() >= 1);
    CHECK(harness.wire->tx.back() == std::vector<std::uint8_t>{sync_response, 1, empty_sync, 0, 0, 0});
    harness.wire->clear_tx();

    auto rejected = write.command;
    ++rejected.back();
    harness.command_frame(rejected, 2);
    CHECK(decode_reply(harness.wire->tx, 2) == std::vector<std::uint8_t>{'N'});
    CHECK(harness.device->requests.empty());
    harness.wire->clear_tx();

    harness.command_frame(write.command, 3);
    harness.wire->clear_tx();
    harness.write_data(write.payload, 4);
    REQUIRE(harness.device->requests.size() == 1);
    harness.wire->clear_tx();

    harness.command_frame(command(0x7f, 'R', static_cast<std::uint16_t>(read.payload.size() - 1)), 5);
    CHECK(decode_reply(harness.wire->tx, 5) == std::vector<std::uint8_t>{'N'});
    harness.wire->clear_tx();
    harness.command_frame(poll.command, 6);
    CHECK(decode_reply(harness.wire->tx, 6) == sio_reply({2, 9, 0}));
    harness.wire->clear_tx();

    harness.wire->push({0xff});
    harness.poll();
    harness.command_frame(command(0x7f, 'P', 0), 7);
    CHECK(decode_reply(harness.wire->tx, 7) == sio_reply({0, 0, 0}));
}
