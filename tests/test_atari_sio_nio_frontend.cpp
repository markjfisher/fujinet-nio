#include "doctest.h"

#include "fujinet/core/core.h"
#include "fujinet/io/atari_sio_nio_frontend.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"

#include "../third_party/cjson/cJSON.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fujinet::io::AtariSioNioFrontend;

struct Scenario {
    std::string id;
    std::array<std::uint8_t, AtariSioNioFrontend::command_bytes> frame{};
    std::vector<std::uint8_t> payload;
    std::uint16_t aux{};
};

std::vector<std::uint8_t> hex_bytes(const char* text)
{
    std::vector<std::uint8_t> out;
    if (!text) return out;
    std::istringstream input(text);
    unsigned value = 0;
    while (input >> std::hex >> value) out.push_back(static_cast<std::uint8_t>(value));
    return out;
}

Scenario fixture(const char* id)
{
    std::ifstream input(std::filesystem::path(FN_ATARI_SIO_FIXTURE_DIR) / "nio_v1.json");
    REQUIRE(input.good());
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    cJSON* root = cJSON_Parse(json.c_str());
    REQUIRE(root != nullptr);
    const cJSON* scenarios = cJSON_GetObjectItemCaseSensitive(root, "scenarios");
    const cJSON* found = nullptr;
    cJSON_ArrayForEach(found, scenarios) {
        const cJSON* candidate = cJSON_GetObjectItemCaseSensitive(found, "id");
        if (cJSON_IsString(candidate) && std::string(candidate->valuestring) == id) break;
    }
    REQUIRE(found != nullptr);

    Scenario out{};
    out.id = id;
    const auto frame = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "frame")));
    REQUIRE(frame.size() == out.frame.size());
    std::copy(frame.begin(), frame.end(), out.frame.begin());
    out.payload = hex_bytes(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(found, "payload")));
    out.aux = static_cast<std::uint16_t>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(found, "aux")));
    cJSON_Delete(root);
    return out;
}

// `dcb_count` in nio_v1.json is a client-side 6502 assertion.  It is not
// available in a physical SIO command frame, so these endpoint tests use only
// the literal frame AUX and payload/checksum fields visible to firmware.

std::array<std::uint8_t, AtariSioNioFrontend::command_bytes>
command(std::uint8_t op, std::uint16_t aux)
{
    std::array<std::uint8_t, AtariSioNioFrontend::command_bytes> out{
        0x7f, op, static_cast<std::uint8_t>(aux & 0xffU), static_cast<std::uint8_t>(aux >> 8U), 0};
    out.back() = AtariSioNioFrontend::checksum(out.data(), out.size() - 1);
    return out;
}

std::vector<std::uint8_t> data_frame(const std::vector<std::uint8_t>& payload)
{
    auto out = payload;
    out.push_back(AtariSioNioFrontend::checksum(payload.data(), payload.size()));
    return out;
}

std::vector<std::uint8_t> data_reply(const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> out{'A', 'C'};
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back(AtariSioNioFrontend::checksum(payload.data(), payload.size()));
    return out;
}

class RecordingDevice final : public fujinet::io::VirtualDevice {
public:
    fujinet::io::IOResponse handle(const fujinet::io::IORequest& request) override
    {
        requests.push_back(request);
        auto response = fujinet::io::make_success_response(request);
        if (oversized) {
            response.payload.assign(600, 0x5a);
        } else if (full_response) {
            response.payload.resize(505);
            for (std::size_t i = 0; i < response.payload.size(); ++i) {
                response.payload[i] = static_cast<std::uint8_t>((i ^ (i >> 8U) ^ 0xa5U) & 0xffU);
            }
        } else {
            response.payload = {0xaa, 0x55};
        }
        return response;
    }

    std::vector<fujinet::io::IORequest> requests;
    bool oversized{false};
    bool full_response{false};
};

struct Harness {
    fujinet::core::FujinetCore core;
    RecordingDevice* device;
    AtariSioNioFrontend frontend;

    Harness()
        : device(new RecordingDevice())
        , frontend(core.routingManager())
    {
        REQUIRE(core.deviceManager().registerDevice(1, std::unique_ptr<fujinet::io::VirtualDevice>(device)));
    }
};

void send_valid_write(Harness& harness, const Scenario& write)
{
    const auto accepted = harness.frontend.begin(write.frame);
    REQUIRE(accepted.accepted);
    REQUIRE(accepted.bytes == std::vector<std::uint8_t>{'A'});
    REQUIRE(accepted.expected_write_bytes == write.payload.size());
    const auto complete = harness.frontend.complete_write(data_frame(write.payload).data(), write.payload.size() + 1);
    REQUIRE(complete.accepted);
    REQUIRE(complete.bytes == std::vector<std::uint8_t>{'A', 'C'});
}

std::vector<std::uint8_t> full_size_request()
{
    fujinet::io::protocol::FujiBusPacket packet{
        static_cast<fujinet::io::protocol::WireDeviceId>(1), 2};
    fujinet::io::protocol::ByteBuffer payload(506);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i ^ (i >> 8U) ^ 0xa5U) & 0xffU);
    }
    return packet.setData(std::move(payload)).serializeRaw();
}

} // namespace

TEST_CASE("Atari host reset reclaims its ingress owner without treating transaction reset as host reset")
{
    struct Route final : fujinet::io::IRequestHandler {
        const void* openedOwner = nullptr;
        const void* resetOwner = nullptr;
        fujinet::io::DeviceID resetDevice = 0;
        unsigned resets = 0;
        fujinet::io::IOResponse handleRequest(const fujinet::io::IORequest& request) override {
            openedOwner = request.clientOwner;
            return fujinet::io::make_success_response(request);
        }
        void resetClient(fujinet::io::DeviceID device, const void* owner) override {
            resetDevice = device;
            resetOwner = owner;
            ++resets;
        }
    } route;
    {
        AtariSioNioFrontend frontend(route);
        const auto write = fixture("w-core-dispatch");
        REQUIRE(frontend.begin(write.frame).accepted);
        const auto frame = data_frame(write.payload);
        REQUIRE(frontend.complete_write(frame.data(), frame.size()).accepted);
        REQUIRE(route.openedOwner != nullptr);
        frontend.reset();
        CHECK(route.resets == 0);
        frontend.reset_host();
        CHECK(route.resets == 1);
        CHECK(route.resetDevice == 0xfd);
        CHECK(route.resetOwner == route.openedOwner);
    }
    CHECK(route.resets == 2); // Destruction cannot leave an owner identity behind.
}

TEST_CASE("Atari native NIO frontend dispatches the literal core packet once and consumes R safely")
{
    Harness harness;
    const auto query = fixture("q-idle");
    const auto write = fixture("w-core-dispatch");
    const auto poll = fixture("p-core-dispatch-ready");
    const auto read = fixture("r-core-dispatch");

    const auto q = harness.frontend.begin(query.frame);
    CHECK(q.accepted);
    CHECK(q.bytes == data_reply({1, 0, 2, 1}));   // flag bit 0: R takes any length
    CHECK(harness.device->requests.empty());
    CHECK(harness.frontend.begin(command('X', 0)).bytes == std::vector<std::uint8_t>{'N'});
    CHECK(harness.frontend.begin(command('P', 1)).bytes == std::vector<std::uint8_t>{'N'});

    send_valid_write(harness, write);
    REQUIRE(harness.device->requests.size() == 1);
    const auto& request = harness.device->requests.front();
    CHECK(request.id == 1);
    CHECK(request.deviceId == 1);
    CHECK(request.command == 2);
    CHECK(request.params.empty());
    CHECK(request.payload.empty());

    const auto ready = harness.frontend.begin(poll.frame);
    CHECK(ready.accepted);
    CHECK(ready.bytes == data_reply({2, 9, 0}));

    const auto busy = harness.frontend.begin(write.frame);
    CHECK_FALSE(busy.accepted);
    CHECK(busy.bytes == std::vector<std::uint8_t>{'N'});
    CHECK(harness.device->requests.size() == 1);

    // A shorter R is refused and the response is kept; so is one over the capacity.
    const auto wrong_read = harness.frontend.begin(command('R', read.aux - 1));
    CHECK_FALSE(wrong_read.accepted);
    CHECK(wrong_read.bytes == std::vector<std::uint8_t>{'N'});
    CHECK_FALSE(harness.frontend.begin(command('R', AtariSioNioFrontend::capacity + 1)).accepted);
    CHECK(harness.frontend.begin(poll.frame).bytes == data_reply({2, 9, 0}));

    const auto terminal = harness.frontend.begin(read.frame);
    REQUIRE(terminal.accepted);
    CHECK(terminal.bytes == data_reply(read.payload));
    CHECK(harness.frontend.begin(fixture("reset-clears").frame).bytes == data_reply({0, 0, 0}));
    // `terminal` remains owned by the result after the slot was consumed.
    CHECK(terminal.bytes == data_reply(read.payload));
}

TEST_CASE("Atari native NIO frontend rejects malformed and oversized W without dispatch")
{
    Harness harness;
    const auto write = fixture("w-core-dispatch");

    auto bad_command = write.frame;
    ++bad_command.back();
    CHECK(harness.frontend.begin(bad_command).bytes == std::vector<std::uint8_t>{'N'});
    CHECK(harness.frontend.begin(command('W', 0)).bytes == std::vector<std::uint8_t>{'N'});

    const auto too_large = harness.frontend.begin(command('W', 513));
    CHECK_FALSE(too_large.accepted);
    CHECK(too_large.bytes == std::vector<std::uint8_t>{'N'});

    const auto receiving = harness.frontend.begin(write.frame);
    REQUIRE(receiving.accepted);
    auto bad_sum = data_frame(write.payload);
    ++bad_sum.back();
    CHECK(harness.frontend.complete_write(bad_sum.data(), bad_sum.size()).bytes ==
          std::vector<std::uint8_t>{'N'});
    CHECK(harness.device->requests.empty());
    CHECK(harness.frontend.begin(fixture("reset-clears").frame).bytes == data_reply({0, 0, 0}));

    // Raw FujiBus declares five bytes but carries six.  Its SIO checksum is
    // valid, so rejection proves packet validation rather than framing alone.
    std::vector<std::uint8_t> malformed{1, 2, 5, 0, 8, 0};
    const auto malformed_command = command('W', malformed.size());
    REQUIRE(harness.frontend.begin(malformed_command).accepted);
    const auto malformed_result = harness.frontend.complete_write(
        data_frame(malformed).data(), malformed.size() + 1);
    CHECK_FALSE(malformed_result.accepted);
    CHECK(malformed_result.bytes == std::vector<std::uint8_t>{'N'});
    CHECK(harness.device->requests.empty());
}

TEST_CASE("Atari native NIO frontend preserves Q and reset boundaries and bounds core responses")
{
    Harness harness;
    const auto write = fixture("w-core-dispatch");

    CHECK(harness.frontend.begin(command('Q', 1)).bytes == std::vector<std::uint8_t>{'N'});
    send_valid_write(harness, write);
    CHECK(harness.frontend.begin(fixture("q-idle").frame).bytes == std::vector<std::uint8_t>{'N'});
    harness.frontend.reset();
    CHECK(harness.frontend.begin(fixture("reset-clears").frame).bytes == data_reply({0, 0, 0}));

    Harness oversized;
    oversized.device->oversized = true;
    send_valid_write(oversized, write);
    const auto ready = oversized.frontend.begin(command('P', 0));
    CHECK(ready.bytes == data_reply({2, 7, 0}));
    const auto response = oversized.frontend.begin(command('R', 7));
    CHECK(response.bytes == data_reply({1, 2, 7, 0, 0x12, 1, 7}));
    CHECK(oversized.device->requests.size() == 1);
}

TEST_CASE("Atari native NIO frontend exchanges a full 512-byte FujiBus packet")
{
    Harness harness;
    harness.device->full_response = true;
    const auto write = fixture("w-512");
    const auto poll = fixture("p-ready-512");
    const auto read = fixture("r-ready-512");
    const auto request = full_size_request();
    REQUIRE(request.size() == AtariSioNioFrontend::capacity);

    const auto accepted = harness.frontend.begin(write.frame);
    REQUIRE(accepted.accepted);
    CHECK(accepted.bytes == std::vector<std::uint8_t>{'A'});
    CHECK(accepted.expected_write_bytes == request.size());
    const auto complete = harness.frontend.complete_write(data_frame(request).data(), request.size() + 1);
    REQUIRE(complete.accepted);
    CHECK(complete.bytes == std::vector<std::uint8_t>{'A', 'C'});
    REQUIRE(harness.device->requests.size() == 1);

    CHECK(harness.frontend.begin(poll.frame).bytes == data_reply({2, 0, 2}));
    const auto terminal = harness.frontend.begin(read.frame);
    REQUIRE(terminal.accepted);
    REQUIRE(terminal.bytes.size() == 2 + AtariSioNioFrontend::capacity + 1);
    CHECK(terminal.bytes[0] == 'A');
    CHECK(terminal.bytes[1] == 'C');
    CHECK(AtariSioNioFrontend::checksum(terminal.bytes.data() + 2,
                                        AtariSioNioFrontend::capacity) == terminal.bytes.back());
    CHECK(harness.frontend.begin(fixture("reset-clears").frame).bytes == data_reply({0, 0, 0}));
}

namespace {
std::uint64_t g_fake_now_us = 0;
std::uint64_t fake_clock() { return g_fake_now_us; }
}

TEST_CASE("Atari native NIO frontend drops a reply nobody read once it is stale")
{
    Harness harness;
    harness.frontend.set_clock(&fake_clock);
    const auto write = fixture("w-core-dispatch");
    const auto poll = fixture("p-core-dispatch-ready");

    g_fake_now_us = 1'000'000;
    send_valid_write(harness, write);
    REQUIRE(harness.device->requests.size() == 1);

    // Fresh: the abandoned reply still holds the endpoint, as the wire contract says.
    g_fake_now_us += AtariSioNioFrontend::stale_reply_us - 1;
    CHECK_FALSE(harness.frontend.begin(write.frame).accepted);
    CHECK(harness.frontend.stale_replies_dropped() == 0);
    // P and R never discard, however old the reply is.
    g_fake_now_us += 10'000'000;
    CHECK(harness.frontend.begin(poll.frame).bytes == data_reply({2, 9, 0}));
    CHECK(harness.frontend.stale_replies_dropped() == 0);

    // Stale: a new W starts a fresh transaction.
    const auto fresh = harness.frontend.begin(write.frame);
    CHECK(fresh.accepted);
    CHECK(harness.frontend.stale_replies_dropped() == 1);
}

TEST_CASE("Atari native NIO frontend without a clock never expires a reply")
{
    Harness harness;
    const auto write = fixture("w-core-dispatch");
    send_valid_write(harness, write);
    CHECK_FALSE(harness.frontend.begin(write.frame).accepted);
    CHECK(harness.frontend.stale_replies_dropped() == 0);
}

TEST_CASE("Atari native NIO frontend pads a longer R with zeros and releases the response")
{
    Harness harness;
    const auto write = fixture("w-core-dispatch");
    const auto read = fixture("r-core-dispatch");
    send_valid_write(harness, write);

    auto padded = read.payload;
    padded.resize(read.payload.size() + 20, 0);
    const auto result = harness.frontend.begin(command('R', static_cast<std::uint16_t>(padded.size())));
    REQUIRE(result.accepted);
    CHECK(result.bytes == data_reply(padded));
    // The slot is idle, but an exact SIO retry must replay the same reply.
    CHECK(harness.frontend.begin(command('P', 0)).bytes == data_reply({0, 0, 0}));
    CHECK(harness.frontend.begin(command('R', static_cast<std::uint16_t>(padded.size()))).bytes == result.bytes);
    CHECK_FALSE(harness.frontend.begin(command('R', static_cast<std::uint16_t>(padded.size() - 1))).accepted);
    REQUIRE(harness.frontend.begin(fixture("w-core-dispatch").frame).accepted);
    CHECK_FALSE(harness.frontend.begin(command('R', static_cast<std::uint16_t>(padded.size()))).accepted);
}

TEST_CASE("Atari native NIO frontend advertises PROC notification in Q flag bit 1 only while it is on")
{
    Harness harness;
    const auto query = fixture("q-idle");
    CHECK(harness.frontend.begin(query.frame).bytes == data_reply({1, 0, 2, 1}));   // off: bit 0 only

    harness.frontend.set_proc_notify(true);
    CHECK(harness.frontend.begin(query.frame).bytes == data_reply({1, 0, 2, 3}));   // bits 0 and 1

    harness.frontend.set_proc_notify(false);
    CHECK(harness.frontend.begin(query.frame).bytes == data_reply({1, 0, 2, 1}));
}

TEST_CASE("Atari native NIO frontend acknowledges a W's data frame before the request runs, when it can")
{
    // The Atari waits about 40 ms for that ACK; a slow request must not delay it.
    struct Order final : fujinet::io::IRequestHandler {
        std::vector<std::string> events;
        fujinet::io::IOResponse handleRequest(const fujinet::io::IORequest& request) override {
            events.push_back("request");
            auto response = fujinet::io::make_success_response(request);
            response.payload = {0xaa};
            return response;
        }
    } order;
    AtariSioNioFrontend frontend(order);
    frontend.set_early_ack([&] { order.events.push_back("ack"); });
    const auto write = fixture("w-core-dispatch");
    REQUIRE(frontend.begin(write.frame).accepted);
    const auto frame = data_frame(write.payload);
    const auto result = frontend.complete_write(frame.data(), frame.size());
    CHECK(result.accepted);
    CHECK(result.bytes == std::vector<std::uint8_t>{'C'});   // the ACK already went out; only COMPLETE is left
    CHECK(order.events == std::vector<std::string>{"ack", "request"});
    // The response is ready as before: P reports it.
    const auto poll = frontend.begin(command('P', 0));
    CHECK(poll.accepted);
    CHECK(poll.bytes.size() == 2 + 3 + 1);
    CHECK(poll.bytes[2] == 2);   // state: ready
}

TEST_CASE("Atari native NIO frontend never acknowledges early a data frame it will refuse")
{
    Harness harness;
    int acks = 0;
    harness.frontend.set_early_ack([&] { ++acks; });
    const auto write = fixture("w-core-dispatch");
    REQUIRE(harness.frontend.begin(write.frame).accepted);
    auto frame = data_frame(write.payload);
    frame.back() ^= 0xff;   // a wrong checksum
    const auto result = harness.frontend.complete_write(frame.data(), frame.size());
    CHECK(result.bytes == std::vector<std::uint8_t>{'N'});
    CHECK(acks == 0);
    CHECK(harness.device->requests.empty());
}

TEST_CASE("Atari native NIO frontend without the early-ACK hook still answers ACK and COMPLETE together")
{
    Harness harness;
    send_valid_write(harness, fixture("w-core-dispatch"));   // checks {'A', 'C'}
}
