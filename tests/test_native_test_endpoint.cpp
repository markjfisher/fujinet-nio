#include "doctest.h"

#include "native_serial_core_parity.h"
#include "native_test_records.h"

#include "fujinet/io/devices/clock_commands.h"
#include "fujinet/io/devices/file_commands.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef FUJINET_NIO_NATIVE_TEST_RUNNER
#define FUJINET_NIO_NATIVE_TEST_RUNNER ""
#endif

using file_clock_core_parity::FrozenUnixTime;
using file_clock_core_parity::decode_raw;
using file_clock_core_parity::make_get_time_format_utc;
using file_clock_core_parity::make_list_request;
using file_clock_core_parity::parse_list_entries;
using file_clock_core_parity::read_u64le;
using fujinet::io::ClockCommand;
using fujinet::io::PacketIOStatus;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FileCommand;
using fujinet::io::protocol::WireDeviceId;
using fujinet::native_test::DirectoryPacketIO;
using fujinet::native_test::DirectoryPacketRole;
using fujinet::native_test::kDirectoryPacketToGuestName;
using fujinet::native_test::kDirectoryPacketToHostName;
using fujinet::native_test::kNativeTestIdentityToken;
using native_test_records::NativeTestClient;
using native_test_records::NativeTestHostStack;
using native_test_records::NativeTestRunnerProcess;
using native_test_records::capture_runner_help;
using native_test_records::make_raw_request;
using native_test_records::make_temp_dir;
using native_test_records::read_identity_file;
using native_test_records::write_bytes;

namespace {

constexpr std::uint64_t kFrozenUnix = 1700000000ULL;
constexpr const char* kExpectedUtcIso = "2023-11-14T22:13:20+0000";
constexpr auto kExchangeTimeout = std::chrono::milliseconds(2000);

const char* runner_path()
{
    return FUJINET_NIO_NATIVE_TEST_RUNNER;
}

bool contains_ci(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

bool identity_is_native_test(const std::string& text)
{
    return contains_ci(text, kNativeTestIdentityToken);
}

bool claims_wrong_backend(const std::string& text)
{
    return contains_ci(text, "Zorro + FujiBus") ||
           contains_ci(text, "packet-native channel (stub)") ||
           contains_ci(text, "FujiBus over SLIP") ||
           contains_ci(text, "FujiBus over TCP") ||
           contains_ci(text, "FujiBus over PTY") ||
           contains_ci(text, "SlipFramer") ||
           contains_ci(text, "FN_BUILD_ZORRO") ||
           contains_ci(text, "PTY placeholder");
}

void seed_host_tree(const std::filesystem::path& directory)
{
    const auto root = directory / "host-fs";
    std::filesystem::create_directories(root / "beta");
    REQUIRE(write_bytes(root / "alpha.txt", {'h', 'i'}));
}

} // namespace

TEST_SUITE("native_test_endpoint") {

TEST_CASE("happy exchange: independent client gets production clock and file-list records")
{
    FrozenUnixTime frozen(kFrozenUnix);
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    seed_host_tree(dir);

    NativeTestHostStack stack(dir.string(), 2048);
    REQUIRE(stack.ready());
    NativeTestClient client(dir.string(), 2048);

    const auto clockReq = make_raw_request(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    const auto clockRaw = stack.exchange(client, clockReq, kExchangeTimeout);
    REQUIRE(clockRaw);
    const auto clock = decode_raw(file_clock_core_parity::ByteBuffer(clockRaw->begin(),
                                                                     clockRaw->end()));
    REQUIRE(clock.got);
    CHECK(clock.status == StatusCode::Ok);
    CHECK(clock.device == static_cast<std::uint8_t>(WireDeviceId::Clock));
    CHECK(clock.command == static_cast<std::uint8_t>(ClockCommand::GetTime));
    REQUIRE(clock.payload.size() >= 12);
    CHECK(read_u64le(clock.payload, 4) == kFrozenUnix);

    const auto fmtReq = make_raw_request(
        WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::GetTimeFormat),
        make_get_time_format_utc());
    const auto fmtRaw = stack.exchange(client, fmtReq, kExchangeTimeout);
    REQUIRE(fmtRaw);
    const auto fmt = decode_raw(file_clock_core_parity::ByteBuffer(fmtRaw->begin(),
                                                                   fmtRaw->end()));
    REQUIRE(fmt.got);
    CHECK(fmt.status == StatusCode::Ok);
    REQUIRE(fmt.payload.size() >= 2);
    const std::string iso(fmt.payload.begin() + 2, fmt.payload.end());
    CHECK(iso == kExpectedUtcIso);

    const auto listReq = make_raw_request(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        make_list_request("host:/"));
    const auto listRaw = stack.exchange(client, listReq, kExchangeTimeout);
    REQUIRE(listRaw);
    const auto list = decode_raw(file_clock_core_parity::ByteBuffer(listRaw->begin(),
                                                                    listRaw->end()));
    REQUIRE(list.got);
    CHECK(list.status == StatusCode::Ok);
    auto entries = parse_list_entries(list.payload);
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
    CHECK(stack.channel().byteCalls == 0);

    std::filesystem::remove_all(dir);
}

TEST_CASE("real disk endpoint reads and flushes slot eight with independent backing verification")
{
    bool subprocess = false;
    SUBCASE("helper registers DiskDevice") {}
    SUBCASE("actual runner registers DiskDevice") { subprocess = true; }

    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    std::filesystem::create_directories(dir / "host-fs");
    const auto image = dir / "host-fs" / "disposable.adf";
    std::vector<std::uint8_t> expected(1760 * 512);
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<std::uint8_t>(i * 37 + i / 512 + 19);
    REQUIRE(write_bytes(image, expected));

    NativeTestRunnerProcess runner;
    std::unique_ptr<NativeTestHostStack> stack;
    if (subprocess) {
        REQUIRE(runner.spawn(runner_path(), dir));
        REQUIRE(runner.wait_for_identity_file(dir, kExchangeTimeout));
    } else {
        stack = std::make_unique<NativeTestHostStack>(dir.string(), 2048);
        REQUIRE(stack->ready());
    }
    NativeTestClient client(dir.string(), 2048);
    auto exchange = [&](std::uint8_t command, const std::vector<std::uint8_t>& payload) {
        const auto request = make_raw_request(WireDeviceId::DiskService, command, payload);
        std::optional<std::vector<std::uint8_t>> raw;
        if (stack) {
            raw = stack->exchange(client, request, kExchangeTimeout);
        } else {
            REQUIRE(client.send_raw(request) == PacketIOStatus::Ok);
            raw = client.wait_record(std::chrono::steady_clock::now() + kExchangeTimeout);
        }
        REQUIRE(raw);
        const auto reply = decode_raw(*raw);
        REQUIRE(reply.got);
        CHECK(reply.device == 0xFC);
        CHECK(reply.command == command);
        return reply;
    };
    auto backing = [&]() {
        std::ifstream input(image, std::ios::binary);
        REQUIRE(input.is_open());
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
    };

    // Literal v1 protocol fields; no production disk payload codec builds expectations.
    const auto empty = exchange(0x05, {1, 8});
    REQUIRE(empty.status == StatusCode::Ok);
    REQUIRE(empty.payload.size() == 13);
    CHECK((empty.payload[1] & 1) == 0);
    const std::string uri = "host:/disposable.adf";
    std::vector<std::uint8_t> mount{1, 8, 0, 0, 0, 0,
                                  static_cast<std::uint8_t>(uri.size()), 0};
    mount.insert(mount.end(), uri.begin(), uri.end());
    const auto mounted = exchange(0x01, mount);
    REQUIRE(mounted.status == StatusCode::Ok);
    CHECK(mounted.payload == std::vector<std::uint8_t>{1, 1, 0, 0, 8, 4, 0, 2, 0xE0, 6, 0, 0});

    const auto initial = exchange(0x03, {1, 8, 17, 0, 0, 0, 0, 2});
    REQUIRE(initial.status == StatusCode::Ok);
    REQUIRE(initial.payload.size() == 523);
    const std::vector<std::uint8_t> readMetadata{1, 0, 0, 0, 8, 17, 0, 0, 0, 0, 2};
    CHECK(std::equal(readMetadata.begin(), readMetadata.end(), initial.payload.begin()));
    CHECK(std::equal(initial.payload.begin() + 11, initial.payload.end(), expected.begin() + 17 * 512));
    CHECK(backing() == expected);

    std::vector<std::uint8_t> write{1, 8, 17, 0, 0, 0, 0, 2};
    for (std::size_t i = 0; i < 512; ++i)
        write.push_back(static_cast<std::uint8_t>(i * 13 + 0xC0));
    REQUIRE(exchange(0x04, write).status == StatusCode::Ok);
    REQUIRE(exchange(0x0E, {1, 8}).status == StatusCode::Ok);
    std::copy(write.begin() + 8, write.end(), expected.begin() + 17 * 512);
    CHECK(backing() == expected); // Entire image, including both sides of the sector.
    const auto reread = exchange(0x03, {1, 8, 17, 0, 0, 0, 0, 2});
    REQUIRE(reread.status == StatusCode::Ok);
    REQUIRE(reread.payload.size() == 523);
    CHECK(std::equal(readMetadata.begin(), readMetadata.end(), reread.payload.begin()));
    CHECK(std::equal(reread.payload.begin() + 11, reread.payload.end(), write.begin() + 8));
    const auto info = exchange(0x05, {1, 8});
    REQUIRE(info.status == StatusCode::Ok);
    REQUIRE(info.payload.size() == 13);
    CHECK((info.payload[1] & 5) == 1); // Still inserted, flush cleared dirty.
    const auto otherSlot = exchange(0x05, {1, 1});
    REQUIRE(otherSlot.status == StatusCode::Ok);
    REQUIRE(otherSlot.payload.size() == 13);
    CHECK((otherSlot.payload[1] & 1) == 0);

    write[2] = 0xE0; write[3] = 6; // LBA 1760 lies beyond the image.
    CHECK(exchange(0x04, write).status == StatusCode::InvalidRequest);
    CHECK(backing() == expected);
    if (stack) CHECK(stack->channel().byteCalls == 0);
    runner.terminate();
    stack.reset();
    CHECK(backing() == expected);
    std::filesystem::remove_all(dir);
}

TEST_CASE("identity: runner banner, profile name, and IDENTITY are native-test")
{
    REQUIRE(std::strlen(runner_path()) > 0);
    const std::string help = capture_runner_help(runner_path());
    REQUIRE_FALSE(help.empty());
    CHECK(identity_is_native_test(help));
    CHECK_FALSE(claims_wrong_backend(help));

    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    NativeTestRunnerProcess runner;
    REQUIRE(runner.spawn(runner_path(), dir));
    REQUIRE(runner.wait_for_identity_file(dir, std::chrono::milliseconds(2000)));
    runner.drain_stdout();

    CHECK(read_identity_file(dir) == std::string(kNativeTestIdentityToken) + "\n");
    CHECK(identity_is_native_test(runner.output()));
    CHECK(contains_ci(runner.output(), "Profile name: native-test"));
    CHECK_FALSE(claims_wrong_backend(runner.output()));

    runner.terminate();
    std::filesystem::remove_all(dir);
}

TEST_CASE("disconnect: removed directory reports Unavailable without opening serial")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    DirectoryPacketIO host(dir.string(), 64, DirectoryPacketRole::Host);
    DirectoryPacketIO client(dir.string(), 64, DirectoryPacketRole::Client);
    fujinet::native_test::DirectoryPacketChannel channel(&host);
    REQUIRE(std::filesystem::remove_all(dir) > 0);

    std::uint8_t buf[64]{};
    const auto rx = host.receive(buf, sizeof(buf));
    CHECK(rx.status == PacketIOStatus::Unavailable);
    CHECK(rx.size == 0);
    const std::uint8_t one[] = {1};
    CHECK(host.send(one, sizeof(one)) == PacketIOStatus::Unavailable);
    CHECK(client.receive(buf, sizeof(buf)).status == PacketIOStatus::Unavailable);
    CHECK(channel.byteCalls == 0);
}

TEST_CASE("stale leftover records are discarded and never complete a later request")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    const std::vector<std::uint8_t> staleHost(16, 0x11);
    const std::vector<std::uint8_t> staleGuest(16, 0x22);
    REQUIRE(write_bytes(dir / kDirectoryPacketToHostName, staleHost));
    REQUIRE(write_bytes(dir / kDirectoryPacketToGuestName, staleGuest));

    NativeTestHostStack stack(dir.string(), 128);
    REQUIRE(stack.ready());
    CHECK_FALSE(std::filesystem::exists(dir / kDirectoryPacketToHostName));
    CHECK_FALSE(std::filesystem::exists(dir / kDirectoryPacketToGuestName));

    NativeTestClient client(dir.string(), 128);
    std::vector<std::uint8_t> unexpected;
    CHECK(client.receive_raw(unexpected).status == PacketIOStatus::NoData);
    CHECK(unexpected.empty());

    const auto req = make_raw_request(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    const auto reply = stack.exchange(client, req, kExchangeTimeout);
    REQUIRE(reply);
    const auto decoded = decode_raw(file_clock_core_parity::ByteBuffer(reply->begin(),
                                                                       reply->end()));
    REQUIRE(decoded.got);
    CHECK(decoded.device == static_cast<std::uint8_t>(WireDeviceId::Clock));
    CHECK(*reply != staleGuest);

    REQUIRE(stack.packets().reset() == PacketIOStatus::Ok);
    CHECK_FALSE(std::filesystem::exists(dir / kDirectoryPacketToHostName));
    CHECK_FALSE(std::filesystem::exists(dir / kDirectoryPacketToGuestName));

    std::filesystem::remove_all(dir);
}

TEST_CASE("oversized record is consumed without delivering a prefix")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    DirectoryPacketIO host(dir.string(), 8, DirectoryPacketRole::Host);
    const std::vector<std::uint8_t> huge(32, 0xAB);
    REQUIRE(write_bytes(dir / kDirectoryPacketToHostName, huge));

    std::uint8_t buf[8]{};
    std::memset(buf, 0x5A, sizeof(buf));
    const auto result = host.receive(buf, sizeof(buf));
    CHECK(result.status == PacketIOStatus::Oversized);
    CHECK(result.size == 0);
    CHECK_FALSE(std::filesystem::exists(dir / kDirectoryPacketToHostName));
    for (unsigned char b : buf) {
        CHECK(b == 0x5A);
    }

    std::uint8_t one[9]{};
    CHECK(host.send(one, sizeof(one)) == PacketIOStatus::Oversized);
    CHECK_FALSE(std::filesystem::exists(dir / kDirectoryPacketToGuestName));

    std::filesystem::remove_all(dir);
}

TEST_CASE("cleanup: runner shutdown leaves packet files gone and no serial listen")
{
    REQUIRE(std::strlen(runner_path()) > 0);
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    seed_host_tree(dir);

    NativeTestRunnerProcess runner;
    REQUIRE(runner.spawn(runner_path(), dir));
    REQUIRE(runner.wait_for_identity_file(dir, std::chrono::milliseconds(2000)));

    NativeTestClient client(dir.string(), 2048);
    const auto req = make_raw_request(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        make_list_request("host:/"));
    REQUIRE(client.send_raw(req) == PacketIOStatus::Ok);
    const auto reply = client.wait_record(std::chrono::steady_clock::now() + kExchangeTimeout);
    REQUIRE(reply);
    const auto decoded = decode_raw(file_clock_core_parity::ByteBuffer(reply->begin(),
                                                                       reply->end()));
    REQUIRE(decoded.got);
    CHECK(decoded.status == StatusCode::Ok);

    auto entries = parse_list_entries(decoded.payload);
    REQUIRE(entries.size() == 2);
    CHECK(std::any_of(entries.begin(), entries.end(),
                      [](const auto& e) { return e.name == "alpha.txt"; }));
    CHECK(std::any_of(entries.begin(), entries.end(),
                      [](const auto& e) { return e.name == "beta"; }));

    runner.terminate();
    CHECK_FALSE(runner.running());
    CHECK_FALSE(std::filesystem::exists(dir / kDirectoryPacketToHostName));
    CHECK_FALSE(std::filesystem::exists(dir / kDirectoryPacketToGuestName));
    CHECK_FALSE(std::filesystem::exists(dir / (std::string(kDirectoryPacketToHostName) + ".tmp")));
    CHECK_FALSE(std::filesystem::exists(dir / (std::string(kDirectoryPacketToGuestName) + ".tmp")));
    runner.drain_stdout();
    CHECK_FALSE(contains_ci(runner.output(), "[ChannelFactory]"));
    CHECK_FALSE(contains_ci(runner.output(), "[SerialChannel]"));
    CHECK_FALSE(contains_ci(runner.output(), "TCP server"));

    std::filesystem::remove_all(dir);
}

TEST_CASE("occupied send slot is Backpressure and leaves the first record")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    DirectoryPacketIO host(dir.string(), 32, DirectoryPacketRole::Host);
    const std::vector<std::uint8_t> first{1, 2, 3, 4};
    const std::vector<std::uint8_t> second{9, 9, 9, 9};
    REQUIRE(host.send(first.data(), first.size()) == PacketIOStatus::Ok);
    CHECK(host.send(second.data(), second.size()) == PacketIOStatus::Backpressure);

    const auto path = dir / kDirectoryPacketToGuestName;
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::vector<std::uint8_t> stored(16);
    const ssize_t n = ::read(fd, stored.data(), stored.size());
    ::close(fd);
    REQUIRE(n == static_cast<ssize_t>(first.size()));
    stored.resize(static_cast<std::size_t>(n));
    CHECK(stored == first);

    std::filesystem::remove_all(dir);
}

TEST_CASE("no fallback: serial and TCP env do not construct SlipFramer or a byte channel")
{
    REQUIRE(std::strlen(runner_path()) > 0);
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    seed_host_tree(dir);

    struct EnvRestore {
        ~EnvRestore()
        {
            ::unsetenv("FN_SERIAL_PORT");
            ::unsetenv("FN_SERIAL_BAUD");
            ::unsetenv("FN_POSIX_LOOP_DELAY_MS");
        }
    } restore;

    ::setenv("FN_SERIAL_PORT", "/dev/ttyUSB0", 1);
    ::setenv("FN_SERIAL_BAUD", "115200", 1);
    ::setenv("FN_POSIX_LOOP_DELAY_MS", "1", 1);

    {
        NativeTestHostStack stack(dir.string(), 2048);
        REQUIRE(stack.ready());
        NativeTestClient inProcess(dir.string(), 2048);
        const auto clockReq = make_raw_request(
            WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
        const auto inReply = stack.exchange(inProcess, clockReq, kExchangeTimeout);
        REQUIRE(inReply);
        CHECK(stack.channel().byteCalls == 0);
    }

    NativeTestRunnerProcess runner;
    REQUIRE(runner.spawn(runner_path(), dir,
                         {"FN_SERIAL_PORT=/dev/ttyUSB0", "FN_SERIAL_BAUD=115200"}));
    REQUIRE(runner.wait_for_identity_file(dir, std::chrono::milliseconds(2000)));

    NativeTestClient client(dir.string(), 2048);
    const auto listReq = make_raw_request(
        WireDeviceId::FileService,
        static_cast<std::uint8_t>(FileCommand::ListDirectory),
        make_list_request("host:/"));
    REQUIRE(client.send_raw(listReq) == PacketIOStatus::Ok);
    const auto reply = client.wait_record(std::chrono::steady_clock::now() + kExchangeTimeout);
    REQUIRE(reply);
    const auto decoded = decode_raw(file_clock_core_parity::ByteBuffer(reply->begin(),
                                                                       reply->end()));
    REQUIRE(decoded.got);
    CHECK(decoded.status == StatusCode::Ok);
    auto entries = parse_list_entries(decoded.payload);
    REQUIRE(entries.size() == 2);

    runner.drain_stdout();
    CHECK_FALSE(contains_ci(runner.output(), "[ChannelFactory]"));
    CHECK_FALSE(contains_ci(runner.output(), "[SerialChannel]"));
    CHECK_FALSE(contains_ci(runner.output(), "SlipFramer"));
    CHECK_FALSE(contains_ci(runner.output(), "TCP server"));
    CHECK_FALSE(contains_ci(runner.output(), "Using PTY channel"));
    CHECK(identity_is_native_test(runner.output()));

    runner.terminate();
    std::filesystem::remove_all(dir);
}

TEST_CASE("client contains timeout and late reply across local reset")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    DirectoryPacketIO host(dir.string(), 128, DirectoryPacketRole::Host);
    NativeTestClient client(dir.string(), 128);
    const auto request = make_raw_request(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    REQUIRE(client.send_raw(request) == PacketIOStatus::Ok);
    std::uint8_t received[128]{};
    REQUIRE(host.receive(received, sizeof(received)).status == PacketIOStatus::Ok);
    // The remote peer owns the request now, although the request file is gone.
    CHECK(client.send_raw(request) == PacketIOStatus::Backpressure);
    CHECK_FALSE(client.wait_record(std::chrono::steady_clock::now()));
    CHECK(client.send_raw(request) == PacketIOStatus::UnknownCompletion);
    REQUIRE(client.reset_local_records() == PacketIOStatus::Ok);
    CHECK(client.send_raw(request) == PacketIOStatus::UnknownCompletion);
    // A remote reply published after local cleanup cannot release containment.
    fujinet::io::protocol::FujiBusPacket late(WireDeviceId::Clock,
        static_cast<std::uint8_t>(ClockCommand::GetTime));
    late.addParamU8(static_cast<std::uint8_t>(StatusCode::Ok));
    late.setData({1, 0, 0, 0, 0, 0xF1, 0x53, 0x65, 0, 0, 0, 0});
    const auto lateRaw = late.serializeRaw();
    REQUIRE(decode_raw(lateRaw).got);
    REQUIRE(host.send(lateRaw.data(), lateRaw.size()) == PacketIOStatus::Ok);
    std::vector<std::uint8_t> out{0xAA};
    CHECK(client.receive_raw(out).status == PacketIOStatus::UnknownCompletion);
    CHECK(out.empty());
    CHECK(client.send_raw(request) == PacketIOStatus::UnknownCompletion);
    CHECK_FALSE(std::filesystem::exists(dir / kDirectoryPacketToHostName));
    std::filesystem::remove_all(dir);
}

TEST_CASE("stale identity does not make a failed launch ready")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    REQUIRE(native_test_records::write_native_test_identity(dir.string()));
    NativeTestRunnerProcess runner;
    REQUIRE(runner.spawn("/nonexistent/native-test-runner", dir));
    CHECK_FALSE(runner.wait_for_identity_file(dir, std::chrono::milliseconds(100)));
    runner.terminate();
    std::filesystem::remove_all(dir);
}

TEST_CASE("runner restart waits for fresh readiness in the same directory")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    NativeTestRunnerProcess runner;
    const auto request = make_raw_request(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    for (int launch = 0; launch < 2; ++launch) {
        REQUIRE(native_test_records::write_native_test_identity(dir.string()));
        REQUIRE(write_bytes(dir / kDirectoryPacketToHostName, {0xAA}));
        REQUIRE(runner.spawn(runner_path(), dir));
        REQUIRE(runner.wait_for_identity_file(dir, kExchangeTimeout));
        CHECK(read_identity_file(dir) == "native-test\n");
        NativeTestClient client(dir.string(), 2048);
        REQUIRE(client.send_raw(request) == PacketIOStatus::Ok);
        auto reply = client.wait_record(std::chrono::steady_clock::now() + kExchangeTimeout);
        REQUIRE(reply);
        REQUIRE(decode_raw(*reply).got);
        runner.terminate();
        CHECK_FALSE(std::filesystem::exists(dir / native_test_records::kDirectoryPacketIdentityName));
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("in-process exchange deadline contains its eventual real reply")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    NativeTestHostStack stack(dir.string(), 128);
    REQUIRE(stack.ready());
    NativeTestClient client(dir.string(), 128);
    const auto request = make_raw_request(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    CHECK_FALSE(stack.exchange(client, request, std::chrono::milliseconds(0)));
    stack.tick(); // The real handler completes after the caller's deadline.
    REQUIRE(std::filesystem::exists(dir / kDirectoryPacketToGuestName));
    std::vector<std::uint8_t> out;
    CHECK(client.receive_raw(out).status == PacketIOStatus::UnknownCompletion);
    CHECK(out.empty());
    REQUIRE(client.reset_local_records() == PacketIOStatus::Ok);
    CHECK(client.send_raw(request) == PacketIOStatus::UnknownCompletion);
    std::filesystem::remove_all(dir);
}

TEST_CASE("receive failure preserves quarantine after local record cleanup")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    DirectoryPacketIO host(dir.string(), 128, DirectoryPacketRole::Host);
    NativeTestClient client(dir.string(), 16);
    const auto request = make_raw_request(
        WireDeviceId::Clock, static_cast<std::uint8_t>(ClockCommand::GetTime), {});
    REQUIRE(client.send_raw(request) == PacketIOStatus::Ok);
    std::uint8_t received[128]{};
    REQUIRE(host.receive(received, sizeof(received)).status == PacketIOStatus::Ok);
    const std::vector<std::uint8_t> oversized(32, 0x55);
    REQUIRE(host.send(oversized.data(), oversized.size()) == PacketIOStatus::Ok);
    std::vector<std::uint8_t> out;
    CHECK(client.receive_raw(out).status == PacketIOStatus::Oversized);
    CHECK(out.empty());
    REQUIRE(client.reset_local_records() == PacketIOStatus::Ok);
    CHECK(client.receive_raw(out).status == PacketIOStatus::UnknownCompletion);
    CHECK(client.send_raw(request) == PacketIOStatus::UnknownCompletion);
    std::filesystem::remove_all(dir);
}

TEST_CASE("live runner readiness requires the exact identity token")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    NativeTestRunnerProcess runner;
    REQUIRE(runner.spawn(runner_path(), dir));
    REQUIRE(runner.wait_for_identity_file(dir, kExchangeTimeout));
    const auto identity = dir / native_test_records::kDirectoryPacketIdentityName;
    REQUIRE(write_bytes(identity, {'x'}));
    CHECK_FALSE(runner.wait_for_identity_file(dir, std::chrono::milliseconds(10)));
    REQUIRE(native_test_records::write_native_test_identity(dir.string()));
    CHECK(runner.wait_for_identity_file(dir, kExchangeTimeout));
    runner.terminate();
    std::filesystem::remove_all(dir);
}

TEST_CASE("runner does not publish readiness when startup cleanup fails")
{
    const auto dir = make_temp_dir();
    REQUIRE_FALSE(dir.empty());
    REQUIRE(std::filesystem::create_directory(dir / kDirectoryPacketToHostName));
    NativeTestRunnerProcess runner;
    REQUIRE(runner.spawn(runner_path(), dir));
    CHECK_FALSE(runner.wait_for_identity_file(dir, kExchangeTimeout));
    CHECK_FALSE(std::filesystem::exists(dir / native_test_records::kDirectoryPacketIdentityName));
    runner.terminate();
    std::filesystem::remove_all(dir);
}

}
