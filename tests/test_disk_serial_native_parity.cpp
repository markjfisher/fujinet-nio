#include "doctest.h"
#include "disk_serial_native_parity.h"

#include "fujinet/io/devices/disk_commands.h"

using fujinet::io::StatusCode;
using fujinet::io::protocol::DiskCommand;
using namespace disk_parity;

namespace {
void check_reply(const ExchangeResult& result, std::uint8_t command,
                 const std::vector<std::uint8_t>& expected,
                 StatusCode status = StatusCode::Ok)
{
    REQUIRE(result.received_request);
    REQUIRE(result.handled);
    REQUIRE(result.framed_ok);
    CHECK(response_matches(result.service, command, status, expected));
    CHECK(response_matches(result.framed, command, status, expected));
    CHECK(result.request_transmissions == 1);
    CHECK(result.request_accepted == 1);
    CHECK(result.reply_attempts == 1);
    CHECK(result.reply_accepted == 1);
}
}

TEST_SUITE("Disk serial/native backing-byte parity") {

TEST_CASE("response oracle rejects corrupt read metadata and every sector byte")
{
    DiskParityWorld world(make_seed_image());
    REQUIRE(exchange_native(world.device, 1, mount_payload(false)).handled);
    REQUIRE(exchange_native(world.device, 4, write_payload(1, unique_sector(0x11))).handled);
    const auto read = exchange_native(world.device, 3, read_payload(1));
    REQUIRE(read_matches_marker_sector(read.framed, 0x11));
    for (std::size_t i = 0; i < read.framed.payload.size(); ++i) {
        auto corrupt = read.framed;
        corrupt.payload[i] ^= 0x80;
        CHECK_FALSE(read_matches_marker_sector(corrupt, 0x11));
    }
    auto wrongDevice = read.framed;
    wrongDevice.deviceId ^= 1;
    CHECK_FALSE(read_matches_marker_sector(wrongDevice, 0x11));
    auto wrongCommand = read.framed;
    wrongCommand.command ^= 1;
    CHECK_FALSE(read_matches_marker_sector(wrongCommand, 0x11));
    auto wrongStatus = read.framed;
    wrongStatus.status = StatusCode::InvalidRequest;
    CHECK_FALSE(read_matches_marker_sector(wrongStatus, 0x11));
    auto truncated = read.framed;
    truncated.payload.pop_back();
    CHECK_FALSE(read_matches_marker_sector(truncated, 0x11));
    auto extended = read.framed;
    extended.payload.push_back(0);
    CHECK_FALSE(read_matches_marker_sector(extended, 0x11));
}

TEST_CASE("read write flush responses and backing bytes match independent expectations")
{
    const auto seed = make_seed_image();
    const auto marker = unique_sector(0x11);
    const auto expected = expected_after_write(seed, 1, marker);

    auto run_path = [&](auto exchange) {
        DiskParityWorld world(seed);
        const auto mount = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::Mount),
                                    mount_payload(false));
        REQUIRE(mount.received_request);
        REQUIRE(mount.handled);
        REQUIRE(mount.service.status == StatusCode::Ok);
        REQUIRE(mount.framed_ok);
        CHECK(mount.framed.status == StatusCode::Ok);
        check_reply(mount, 1, {1, 1, 0, 0, 1, 4, 0, 1, 4, 0, 0, 0});

        const auto read0 = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::ReadSector),
                                    read_payload(0));
        REQUIRE(read0.service.status == StatusCode::Ok);
        REQUIRE(read0.framed_ok);
        REQUIRE(read0.service.payload.size() >= 11 + kSectorSize);
        auto expectedRead0 = expected_sector_reply(0);
        expectedRead0.insert(expectedRead0.end(), seed.begin(), seed.begin() + kSectorSize);
        check_reply(read0, 3, expectedRead0);

        const auto write = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::WriteSector),
                                    write_payload(1, marker));
        REQUIRE(write.service.status == StatusCode::Ok);
        REQUIRE(write.framed_ok);
        check_reply(write, 4, expected_sector_reply(1));
        CHECK(world.write_effects() == 1);
        CHECK(world.image() == expected);
        CHECK(neighbors_unchanged(seed, world.image(), 1));

        const auto read1 = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::ReadSector),
                                    read_payload(1));
        REQUIRE(read1.service.status == StatusCode::Ok);
        CHECK(read_matches_marker_sector(read1.service, 0x11));
        CHECK(read_matches_marker_sector(read1.framed, 0x11));
        auto expectedRead1 = expected_sector_reply(1);
        expectedRead1.insert(expectedRead1.end(), marker.begin(), marker.end());
        check_reply(read1, 3, expectedRead1);

        const auto flushes_before = world.flush_count();
        const auto flush = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::Flush),
                                    flush_payload());
        REQUIRE(flush.service.status == StatusCode::Ok);
        REQUIRE(flush.framed_ok);
        check_reply(flush, 0x0E, {1, 0, 0, 0, 1});
        CHECK(world.flush_count() == flushes_before + 1);
        CHECK(world.image() == expected);
        CHECK(neighbors_unchanged(seed, world.image(), 1));
        CHECK(world.write_effects() == 1);
        return std::vector<IOResponse>{mount.framed, read0.framed, write.framed, read1.framed, flush.framed};
    };

    const auto serial = run_path([](DiskDevice& dev, std::uint8_t cmd, const std::vector<std::uint8_t>& payload) {
        return exchange_serial(dev, cmd, payload);
    });
    const auto native = run_path([](DiskDevice& dev, std::uint8_t cmd, const std::vector<std::uint8_t>& payload) {
        return exchange_native(dev, cmd, payload);
    });
    REQUIRE(serial.size() == native.size());
    for (std::size_t i = 0; i < serial.size(); ++i) {
        CHECK(response_matches(native[i], serial[i].command, serial[i].status, serial[i].payload));
    }

}

TEST_CASE("write protection is a service error with zero write effects")
{
    const auto seed = make_seed_image();
    const auto marker = unique_sector(0x22);

    auto reject = [&](auto exchange) {
        DiskParityWorld world(seed);
        REQUIRE(exchange(world.device, static_cast<std::uint8_t>(DiskCommand::Mount),
                         mount_payload(true)).service.status == StatusCode::Ok);
        const auto write = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::WriteSector),
                                    write_payload(1, marker));
        REQUIRE(write.handled);
        CHECK(write.service.status == StatusCode::InvalidRequest);
        check_reply(write, 4, {}, StatusCode::InvalidRequest);
        CHECK(write.framed_ok);
        CHECK(write.framed.status == StatusCode::InvalidRequest);
        CHECK(write.request_transmissions == 1);
        CHECK(write.request_accepted == 1);
        CHECK(write.reply_attempts == 1);
        CHECK(world.write_effects() == 0);
        CHECK(world.image() == seed);
        CHECK(neighbors_unchanged(seed, world.image(), 1));
    };

    reject([](DiskDevice& dev, std::uint8_t cmd, const std::vector<std::uint8_t>& payload) {
        return exchange_serial(dev, cmd, payload);
    });
    reject([](DiskDevice& dev, std::uint8_t cmd, const std::vector<std::uint8_t>& payload) {
        return exchange_native(dev, cmd, payload);
    });
}

TEST_CASE("out-of-range requests are service errors with unchanged backing bytes")
{
    const auto seed = make_seed_image();
    const auto marker = unique_sector(0x33);

    auto reject = [&](auto exchange) {
        DiskParityWorld world(seed);
        REQUIRE(exchange(world.device, static_cast<std::uint8_t>(DiskCommand::Mount),
                         mount_payload(false)).service.status == StatusCode::Ok);
        const auto write = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::WriteSector),
                                    write_payload(kSectorCount, marker));
        REQUIRE(write.handled);
        CHECK(write.service.status == StatusCode::InvalidRequest);
        check_reply(write, 4, {}, StatusCode::InvalidRequest);
        CHECK(write.framed.status == StatusCode::InvalidRequest);
        CHECK(world.write_effects() == 0);
        CHECK(world.image() == seed);

        const auto read = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::ReadSector),
                                   read_payload(kSectorCount));
        REQUIRE(read.handled);
        CHECK(read.service.status == StatusCode::InvalidRequest);
        check_reply(read, 3, {}, StatusCode::InvalidRequest);
        CHECK(world.image() == seed);
        CHECK(neighbors_unchanged(seed, world.image(), 0));
    };

    reject([](DiskDevice& dev, std::uint8_t cmd, const std::vector<std::uint8_t>& payload) {
        return exchange_serial(dev, cmd, payload);
    });
    reject([](DiskDevice& dev, std::uint8_t cmd, const std::vector<std::uint8_t>& payload) {
        return exchange_native(dev, cmd, payload);
    });
}

TEST_CASE("failed request delivery never reaches DiskDevice and has zero write effects")
{
    const auto seed = make_seed_image();
    DiskParityWorld world(seed);
    REQUIRE(exchange_native(world.device, static_cast<std::uint8_t>(DiskCommand::Mount),
                            mount_payload(false)).service.status == StatusCode::Ok);
    const auto before = world.write_effects();

    const auto blocked = exchange_native(
        world.device,
        static_cast<std::uint8_t>(DiskCommand::WriteSector),
        write_payload(1, unique_sector(0x44)),
        NativeRequestFate::Unavailable,
        NativeReplyFate::Deliver);

    CHECK_FALSE(blocked.received_request);
    CHECK_FALSE(blocked.handled);
    CHECK(blocked.request_transmissions == 1);
    CHECK(blocked.request_send_status == fujinet::io::PacketIOStatus::Unavailable);
    CHECK(blocked.reply_send_status == fujinet::io::PacketIOStatus::NoData);
    CHECK(blocked.request_accepted == 0);
    CHECK(blocked.reply_attempts == 0);
    CHECK(blocked.reply_accepted == 0);
    CHECK(world.write_effects() == before);
    CHECK(world.image() == seed);
}

TEST_CASE("lost or ambiguous reply after write keeps one effect and one transmission")
{
    const auto seed = make_seed_image();
    const auto marker = unique_sector(0x55);
    const auto expected = expected_after_write(seed, 1, marker);

    auto after_write = [&](NativeReplyFate fate) {
        DiskParityWorld world(seed);
        REQUIRE(exchange_native(world.device, static_cast<std::uint8_t>(DiskCommand::Mount),
                                mount_payload(false)).service.status == StatusCode::Ok);
        const auto write = exchange_native(
            world.device,
            static_cast<std::uint8_t>(DiskCommand::WriteSector),
            write_payload(1, marker),
            NativeRequestFate::Deliver,
            fate);
        REQUIRE(write.handled);
        REQUIRE(write.service.status == StatusCode::Ok);
        CHECK(response_matches(write.service, 4, StatusCode::Ok, expected_sector_reply(1)));
        CHECK_FALSE(write.framed_ok);
        CHECK(write.request_send_status == fujinet::io::PacketIOStatus::Ok);
        CHECK(write.reply_send_status == (fate == NativeReplyFate::SendFailed
            ? fujinet::io::PacketIOStatus::SendFailed
            : fujinet::io::PacketIOStatus::UnknownCompletion));
        CHECK(write.request_transmissions == 1);
        CHECK(write.request_accepted == 1);
        CHECK(write.reply_attempts == 1);
        CHECK(world.write_effects() == 1);
        CHECK(world.image() == expected);
        CHECK(world.image()[kSectorSize + 1] == 0x55);
        CHECK(neighbors_unchanged(seed, world.image(), 1));
        return write.reply_accepted;
    };

    CHECK(after_write(NativeReplyFate::SendFailed) == 0);
    CHECK(after_write(NativeReplyFate::UnknownCompletion) == 1);
}

TEST_CASE("effect counts expose a second write and a blocked replay")
{
    const auto seed = make_seed_image();
    const auto first = unique_sector(0x66);
    const auto second = unique_sector(0x77);

    DiskParityWorld executed(seed);
    REQUIRE(exchange_native(executed.device, static_cast<std::uint8_t>(DiskCommand::Mount),
                            mount_payload(false)).service.status == StatusCode::Ok);
    REQUIRE(exchange_native(executed.device, static_cast<std::uint8_t>(DiskCommand::WriteSector),
                            write_payload(1, first)).service.status == StatusCode::Ok);
    CHECK(executed.write_effects() == 1);
    CHECK(executed.image()[kSectorSize + 1] == 0x66);

    REQUIRE(exchange_native(executed.device, static_cast<std::uint8_t>(DiskCommand::WriteSector),
                            write_payload(1, second)).service.status == StatusCode::Ok);
    CHECK(executed.write_effects() == 2);
    CHECK(executed.image()[kSectorSize + 1] == 0x77);
    CHECK(neighbors_unchanged(seed, executed.image(), 1));

    DiskParityWorld blocked(seed);
    REQUIRE(exchange_native(blocked.device, static_cast<std::uint8_t>(DiskCommand::Mount),
                            mount_payload(false)).service.status == StatusCode::Ok);
    REQUIRE(exchange_native(blocked.device, static_cast<std::uint8_t>(DiskCommand::WriteSector),
                            write_payload(1, first)).service.status == StatusCode::Ok);
    const auto replay = exchange_native(
        blocked.device,
        static_cast<std::uint8_t>(DiskCommand::WriteSector),
        write_payload(1, second),
        NativeRequestFate::Unavailable,
        NativeReplyFate::Deliver);
    CHECK_FALSE(replay.handled);
    CHECK(replay.request_transmissions == 1);
    CHECK(replay.request_accepted == 0);
    CHECK(replay.reply_attempts == 0);
    CHECK(blocked.write_effects() == 1);
    CHECK(blocked.image()[kSectorSize + 1] == 0x66);
    CHECK(blocked.image() != expected_after_write(seed, 1, second));
}

} // TEST_SUITE
