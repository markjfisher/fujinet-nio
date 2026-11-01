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
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
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
    void clear_tx() { tx.clear(); tx_times.clear(); }
    bool available() override { return !rx.empty(); }
    bool supports_readable_wait() const override { return wait_enabled; }
    bool wait_for_readable(std::chrono::milliseconds timeout) override {
        ++wait_calls;
        last_wait = timeout;
        while (!after_wait.empty()) {
            rx.push_back(std::move(after_wait.front()));
            after_wait.pop_front();
        }
        return !rx.empty();
    }
    std::size_t read(std::uint8_t* buffer, std::size_t max_len) override
    {
        if (rx.empty()) return 0;
        const auto datagram = std::move(rx.front());
        rx.pop_front();
        REQUIRE(datagram.size() <= max_len);
        std::memcpy(buffer, datagram.data(), datagram.size());
        return datagram.size();
    }
    void write(const std::uint8_t* buffer, std::size_t len) override {
        tx.emplace_back(buffer, buffer + len);
        tx_times.push_back(std::chrono::steady_clock::now());
    }

    std::deque<std::vector<std::uint8_t>> rx;
    std::deque<std::vector<std::uint8_t>> after_wait;
    bool wait_enabled{false};
    unsigned wait_calls{0};
    std::chrono::milliseconds last_wait{0};
    std::vector<std::vector<std::uint8_t>> tx;
    std::vector<std::chrono::steady_clock::time_point> tx_times;
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

DatagramChannel* delay_wire = nullptr;
std::vector<std::uint32_t> delay_values;
std::vector<std::vector<std::uint8_t>> delay_preceding;
void record_delay(std::uint32_t value)
{
    delay_values.push_back(value);
    REQUIRE(delay_wire != nullptr);
    REQUIRE(!delay_wire->tx.empty());
    delay_preceding.push_back(delay_wire->tx.back());
}
std::uint64_t test_clock_us = 0;
std::uint64_t test_clock() { return test_clock_us; }

struct Harness {
    fujinet::fs::StorageManager storage;
    fujinet::disk::DiskService disks{storage, fujinet::disk::make_default_image_registry()};
    fujinet::core::FujinetCore core;
    RecordingDevice* device{new RecordingDevice()};
    DatagramChannel* wire{new DatagramChannel()};
    std::unique_ptr<fujinet::platform::posix::AtariNetSioIngress> ingress;

    explicit Harness(bool use_fake_clock = false, bool fake_delay = false, std::uint32_t gap = 250)
    {
        REQUIRE(core.deviceManager().registerDevice(1, std::unique_ptr<fujinet::io::VirtualDevice>(device)));
        if (use_fake_clock || fake_delay) {
            delay_wire = wire;
            ingress = std::make_unique<fujinet::platform::posix::AtariNetSioIngress>(
                std::unique_ptr<fujinet::io::Channel>(wire), disks, core.routingManager(),
                use_fake_clock ? test_clock : nullptr, gap, fake_delay ? record_delay : nullptr);
        } else {
            ingress = fujinet::platform::posix::create_atari_netsio_ingress(
                std::unique_ptr<fujinet::io::Channel>(wire), disks, core.routingManager());
        }
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
    CHECK(decode_reply(harness.wire->tx, 1) == sio_reply({1, 0, 2, 1}));
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

TEST_CASE("Atari NetSIO expires a lost reply after a suspended client resumes")
{
    test_clock_us = 100;
    Harness harness(true);
    const auto write = fixture("w-core-dispatch");
    harness.command_frame(write.command, 1);
    require_write_ack(harness, 1, write.payload.size());
    harness.wire->clear_tx();
    harness.write_data(write.payload, 2);
    REQUIRE(harness.device->requests.size() == 1);
    harness.wire->clear_tx();

    // An unconsumed fresh reply protects the completed request.
    test_clock_us += fujinet::io::AtariSioNioFrontend::stale_reply_us - 1;
    harness.command_frame(write.command, 3);
    CHECK(decode_reply(harness.wire->tx, 3) == std::vector<std::uint8_t>{'N'});
    CHECK(harness.device->requests.size() == 1);
    harness.wire->clear_tx();

    // After suspension beyond the deadline, the same write can proceed.
    ++test_clock_us;
    harness.command_frame(write.command, 4);
    require_write_ack(harness, 4, write.payload.size());
    harness.wire->clear_tx();
    harness.write_data(write.payload, 5);
    CHECK(decode_reply(harness.wire->tx, 5) == std::vector<std::uint8_t>{'A', 'C'});
    CHECK(harness.device->requests.size() == 2);
}

TEST_CASE("Atari NetSIO production clock expires an abandoned reply")
{
    Harness harness;
    const auto write = fixture("w-core-dispatch");
    harness.command_frame(write.command, 1);
    require_write_ack(harness, 1, write.payload.size());
    harness.wire->clear_tx();
    harness.write_data(write.payload, 2);
    REQUIRE(harness.device->requests.size() == 1);
    harness.wire->clear_tx();
    std::this_thread::sleep_for(std::chrono::microseconds(
        fujinet::io::AtariSioNioFrontend::stale_reply_us + 100'000));
    harness.command_frame(write.command, 3);
    require_write_ack(harness, 3, write.payload.size());
}

TEST_CASE("Atari NetSIO spaces ACK and completion using configured T5")
{
    delay_values.clear();
    delay_preceding.clear();
    Harness h(false, true, 375);
    const auto write = fixture("w-core-dispatch");
    const auto poll = fixture("p-core-dispatch-ready");
    const auto read = fixture("r-core-dispatch");
    h.command_frame(fixture("q-idle").command, 1);
    REQUIRE(delay_values == std::vector<std::uint32_t>{375});
    CHECK(delay_preceding.back() == std::vector<std::uint8_t>{sync_response, 1, ack_sync, 'A', 0, 0});
    CHECK(decode_reply(h.wire->tx, 1) == sio_reply({1, 0, 2, 1}));
    h.wire->clear_tx();
    h.command_frame(write.command, 2); // ACK starts the data phase: no completion yet.
    CHECK(delay_values.size() == 1);
    h.wire->clear_tx();
    h.write_data(write.payload, 3);
    REQUIRE(delay_values.size() == 2);
    CHECK(delay_preceding.back() == std::vector<std::uint8_t>{sync_response, 3, ack_sync, 'A', 0, 0});
    CHECK(decode_reply(h.wire->tx, 3) == std::vector<std::uint8_t>{'A', 'C'});
    h.wire->clear_tx();
    h.command_frame(poll.command, 4);
    h.wire->clear_tx();
    h.command_frame(read.command, 5);
    CHECK(delay_values == std::vector<std::uint32_t>{375, 375, 375, 375});
    h.wire->clear_tx();
    h.command_frame(command(0x7f, '?', 0), 6);
    CHECK(decode_reply(h.wire->tx, 6) == std::vector<std::uint8_t>{'N'});
    CHECK(delay_values.size() == 4);
}

TEST_CASE("Atari NetSIO allows the ACK gap to be disabled")
{
    delay_values.clear();
    Harness h(false, true, 0);
    h.command_frame(fixture("q-idle").command, 1);
    CHECK(decode_reply(h.wire->tx, 1) == sio_reply({1, 0, 2, 1}));
    CHECK(delay_values.empty());
}

TEST_CASE("Atari NetSIO default production gap separates datagrams")
{
    Harness h;
    h.command_frame(fixture("q-idle").command, 9);
    std::size_t ack = h.wire->tx.size(), complete = h.wire->tx.size();
    for (std::size_t i = 0; i < h.wire->tx.size(); ++i) {
        const auto& packet = h.wire->tx[i];
        if (packet.size() == 6 && packet[0] == sync_response && packet[1] == 9 && packet[3] == 'A') ack = i;
        if (packet.size() == 2 && packet[0] == data_byte && packet[1] == 'C') complete = i;
    }
    REQUIRE(ack < complete);
    REQUIRE(complete < h.wire->tx_times.size());
    CHECK(h.wire->tx_times[complete] - h.wire->tx_times[ack] >= std::chrono::microseconds(250));
}

TEST_CASE("Atari NetSIO waits for delayed credit and preserves unrelated datagrams")
{
    Harness h;
    h.wire->wait_enabled = true;
    h.wire->after_wait.push_back({0xc2}); // ping: process after the SIO reply
    h.wire->after_wait.push_back({0xc7, 3});
    h.command_frame(fixture("q-idle").command, 10);
    CHECK(h.wire->wait_calls == 1);
    CHECK(h.wire->last_wait > std::chrono::milliseconds::zero());
    CHECK(h.wire->last_wait <= std::chrono::milliseconds(5));
    CHECK(decode_reply(h.wire->tx, 10) == sio_reply({1, 0, 2, 1}));
    CHECK(std::find(h.wire->tx.begin(), h.wire->tx.end(),
                    std::vector<std::uint8_t>{0xc3}) != h.wire->tx.end());
    CHECK(h.wire->rx.empty());
}

TEST_CASE("Atari NetSIO answers after the credit wait times out")
{
    Harness h;
    h.wire->wait_enabled = true;
    h.command_frame(fixture("q-idle").command, 11);
    CHECK(h.wire->wait_calls == 1);
    CHECK(decode_reply(h.wire->tx, 11) == sio_reply({1, 0, 2, 1}));
}

TEST_CASE("Atari NetSIO preserves a complete command received while waiting for credit")
{
    Harness h;
    h.wire->wait_enabled = true;
    const auto frame = fixture("q-idle").command;
    h.wire->after_wait.push_back({command_on});
    std::vector<std::uint8_t> block{data_block};
    block.insert(block.end(), frame.begin(), frame.end());
    block.push_back(0);
    h.wire->after_wait.push_back(std::move(block));
    h.wire->after_wait.push_back({command_off_sync, 13});
    h.wire->after_wait.push_back({0xc7, 3});
    h.command_frame(frame, 12);
    const auto second = std::find_if(h.wire->tx.begin(), h.wire->tx.end(), [](const auto& packet) {
        return packet.size() == 6 && packet[0] == sync_response && packet[1] == 13;
    });
    REQUIRE(second != h.wire->tx.end());
    const std::vector<std::vector<std::uint8_t>> first_reply(h.wire->tx.begin(), second);
    const std::vector<std::vector<std::uint8_t>> second_reply(second, h.wire->tx.end());
    CHECK(decode_reply(first_reply, 12) == sio_reply({1, 0, 2, 1}));
    CHECK(decode_reply(second_reply, 13) == sio_reply({1, 0, 2, 1}));
    CHECK(h.wire->rx.empty());
}
