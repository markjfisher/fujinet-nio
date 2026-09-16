#include "doctest.h"
#include "disk_serial_native_parity.h"

#include "fujinet/io/devices/disk_commands.h"

using fujinet::io::StatusCode;
using fujinet::io::protocol::DiskCommand;
using namespace disk_parity;

TEST_SUITE("Disk serial/native backing-byte parity") {

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

        const auto read0 = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::ReadSector),
                                    read_payload(0));
        REQUIRE(read0.service.status == StatusCode::Ok);
        REQUIRE(read0.framed_ok);
        REQUIRE(read0.service.payload.size() >= 11 + kSectorSize);
        CHECK(read0.service.payload[11] == 0xA0);

        const auto write = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::WriteSector),
                                    write_payload(1, marker));
        REQUIRE(write.service.status == StatusCode::Ok);
        REQUIRE(write.framed_ok);
        CHECK(world.write_effects() == 1);
        CHECK(world.image() == expected);
        CHECK(neighbors_unchanged(seed, world.image(), 1));

        const auto read1 = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::ReadSector),
                                    read_payload(1));
        REQUIRE(read1.service.status == StatusCode::Ok);
        CHECK(read_ok_contains(read1.service, 0x11));
        CHECK(read_ok_contains(read1.framed, 0x11));

        const auto flushes_before = world.flush_count();
        const auto flush = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::Flush),
                                    flush_payload());
        REQUIRE(flush.service.status == StatusCode::Ok);
        REQUIRE(flush.framed_ok);
        CHECK(world.flush_count() == flushes_before + 1);
        CHECK(world.image() == expected);
        CHECK(neighbors_unchanged(seed, world.image(), 1));
        CHECK(world.write_effects() == 1);
    };

    run_path([](DiskDevice& dev, std::uint8_t cmd, const std::vector<std::uint8_t>& payload) {
        return exchange_serial(dev, cmd, payload);
    });
    run_path([](DiskDevice& dev, std::uint8_t cmd, const std::vector<std::uint8_t>& payload) {
        return exchange_native(dev, cmd, payload);
    });
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
        CHECK(write.framed_ok);
        CHECK(write.framed.status == StatusCode::InvalidRequest);
        CHECK(write.transmissions >= 1);
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
        CHECK(write.framed.status == StatusCode::InvalidRequest);
        CHECK(world.write_effects() == 0);
        CHECK(world.image() == seed);

        const auto read = exchange(world.device, static_cast<std::uint8_t>(DiskCommand::ReadSector),
                                   read_payload(kSectorCount));
        REQUIRE(read.handled);
        CHECK(read.service.status == StatusCode::InvalidRequest);
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
    CHECK(blocked.transmissions == 0);
    CHECK(blocked.accepted == 0);
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
        CHECK_FALSE(write.framed_ok);
        CHECK(write.transmissions == 1);
        CHECK(world.write_effects() == 1);
        CHECK(world.image() == expected);
        CHECK(world.image()[kSectorSize + 1] == 0x55);
        CHECK(neighbors_unchanged(seed, world.image(), 1));
        return write.accepted;
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
    CHECK(replay.transmissions == 0);
    CHECK(blocked.write_effects() == 1);
    CHECK(blocked.image()[kSectorSize + 1] == 0x66);
    CHECK(blocked.image() != expected_after_write(seed, 1, second));
}

} // TEST_SUITE
