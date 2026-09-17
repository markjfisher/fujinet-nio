#include "disk_catalog_serial_native_parity.h"

namespace {
using namespace catalog_parity;
constexpr const char* a = "host:/disks/a.adf";
constexpr const char* b = "host:/disks/b.adf";
constexpr const char* hd = "host:/disks/hd.adf";

void populate(World& w)
{
    // Canonicalize a relative selection through the established current host.
    fujinet::io::AppStore::WriteResult written{};
    const std::string current = "host:/disks";
    REQUIRE(w.store->write("fujinet-nio", "current-host", 0,
        reinterpret_cast<const std::uint8_t*>(current.data()), current.size(), written));
    w.expect(0xf2, 2, put(100, false, "a.adf"), StatusCode::Ok, entry(100, false, a));
    w.expect(0xf2, 2, put(101, false, b), StatusCode::Ok, entry(101, false, b));
    w.expect(0xf2, 2, put(200, true, hd), StatusCode::Ok, entry(200, true, hd));
    w.select(100, 1, false, a, false);
    w.select(200, 8, true, hd, true);
    w.simple(6, 8);
}
void other_slot(World& w)
{
    w.info(8, geometry(8, 0x33, true, true));
    w.read(8, seed(true, 0x82));
}

TEST_SUITE("disk_catalog_serial_native_parity") {
TEST_CASE("Catalogue selection and DD DD HD DD replacement preserve independent slots")
{
    for (bool native : {false, true}) {
        World w(native);
        populate(w);
        w.info(1, geometry(1, 0x39, false, true));
        other_slot(w);
        w.backing();
        w.simple(6, 1);
        w.info(1, geometry(1, 0x31, false, true));
        other_slot(w);
        w.select(101, 1, false, b, false);
        w.runtime("v1\n0\t0\trw\thost:/disks/b.adf\n7\t0\tr\thost:/disks/hd.adf\n");
        w.info(1, geometry(1, 0x39, false, true));
        w.read(1, seed(false, 0x61));
        other_slot(w); w.backing();
        w.simple(6, 1);
        w.select(200, 1, true, hd, true);
        w.runtime("v1\n0\t0\tr\thost:/disks/hd.adf\n7\t0\tr\thost:/disks/hd.adf\n");
        other_slot(w); w.backing();
        w.recreate();
        CHECK(w.disk->restore_runtime_mounts() == std::vector<std::size_t>{0, 7});
        w.info(1, geometry(1, 0x3b, true, true));
        w.info(8, geometry(8, 0x3b, true, true));
        w.simple(6, 8);
        w.read(1, seed(true, 0x82), 2000);
        w.read(1, seed(true, 0x82), 3519);
        w.info(1, geometry(1, 0x3b, true, true));
        w.read(1, seed(true, 0x82));
        other_slot(w); w.backing();
        w.simple(6, 1);
        w.select(100, 1, false, a, false);
        w.info(1, geometry(1, 0x39, false, true));
        w.read(1, seed(false, 0x40));
        other_slot(w); w.backing();
        w.runtime("v1\n0\t0\trw\thost:/disks/a.adf\n7\t0\tr\thost:/disks/hd.adf\n");
    }
}

TEST_CASE("Write flush eject remount and reconstructed firmware retain bytes catalogue and modes")
{
    for (bool native : {false, true}) {
        World w(native);
        populate(w);
        w.simple(6, 1);
        Bytes data(512, 0xa5);
        data[0] = 0xc0; data[1] = 0xdb; // exercise SLIP escaping too
        auto write = sector_request(1);
        write.insert(write.end(), data.begin(), data.end());
        w.expect(0xfc, 4, write, StatusCode::Ok, sector_reply(1));
        w.info(1, geometry(1, 0x35, false, true));
        auto expected = seed(false, 0x40);
        std::copy(data.begin(), data.end(), expected.begin() + 3 * 512);
        w.backing(expected); other_slot(w);
        const auto flushes = w.fs->flush_count();
        w.simple(0x0e, 1);
        CHECK(w.fs->flush_count() == flushes + 1);
        w.info(1, geometry(1, 0x31, false, true));
        w.read(1, expected); w.backing(expected); other_slot(w);
        w.simple(2, 1);
        w.info(1, empty_info(1));
        w.expect(0xfc, 3, sector_request(1), StatusCode::NotReady, {});
        w.runtime("v1\n7\t0\tr\thost:/disks/hd.adf\n");
        other_slot(w); w.backing(expected);
        w.select(100, 1, false, a, false);
        w.read(1, expected);
        w.info(1, geometry(1, 0x39, false, true));

        w.recreate();
        const auto restored = w.disk->restore_runtime_mounts();
        CHECK(restored == std::vector<std::size_t>{0, 7});
        for (const auto index : {0u, 7u}) {
            CHECK_FALSE(w.disk->disk_service().info(index).inserted);
            CHECK(w.disk->disk_service().get_pending_mount(index).has_value());
        }
        w.expect(0xf2, 1, {1, 100}, StatusCode::Ok, entry(100, false, a));
        w.expect(0xf2, 1, {1, 101}, StatusCode::Ok, entry(101, false, b));
        w.expect(0xf2, 1, {1, 200}, StatusCode::Ok, entry(200, true, hd));
        w.info(1, geometry(1, 0x39, false, true));
        w.info(8, geometry(8, 0x3b, true, true));
        w.simple(6, 8);
        w.read(1, expected); other_slot(w); w.backing(expected);
        data.assign(512, 0x96);
        write = sector_request(1);
        write.insert(write.end(), data.begin(), data.end());
        w.expect(0xfc, 4, write, StatusCode::Ok, sector_reply(1));
        std::copy(data.begin(), data.end(), expected.begin() + 3 * 512);
        w.info(1, geometry(1, 0x3d, false, true));
        w.simple(0x0e, 1);
        w.read(1, expected); other_slot(w); w.backing(expected);
        write = sector_request(8);
        write.insert(write.end(), data.begin(), data.end());
        w.expect(0xfc, 4, write, StatusCode::InvalidRequest, {});
        w.info(8, geometry(8, 0x33, true, true));
        w.info(1, geometry(1, 0x39, false, true));
        w.backing(expected);

        w.simple(2, 1);
        w.recreate();
        CHECK(w.disk->restore_runtime_mounts() == std::vector<std::size_t>{7});
        CHECK_FALSE(w.disk->disk_service().get_pending_mount(0).has_value());
        w.info(1, empty_info(1, 0x20));
        w.expect(0xfc, 3, sector_request(1), StatusCode::NotReady, {});
        w.info(8, geometry(8, 0x3b, true, true));
        w.simple(6, 8);
        other_slot(w); w.backing(expected);
        w.expect(0xf2, 1, {1, 100}, StatusCode::Ok, entry(100, false, a));
        w.runtime("v1\n7\t0\tr\thost:/disks/hd.adf\n");
    }
}

TEST_CASE("Catalogue errors RO protection and failed replacements keep established state")
{
    for (bool native : {false, true}) {
        World w(native);
        populate(w);
        w.expect(0xf2, 1, {1, 222}, StatusCode::DeviceNotFound, {});
        w.expect(0xf2, 1, {2, 100}, StatusCode::InvalidRequest, {});
        auto invalid = put(100, false, b);
        invalid[2] = 4;
        w.expect(0xf2, 2, invalid, StatusCode::InvalidRequest, {});
        w.expect(0xf2, 1, {1, 100}, StatusCode::Ok, entry(100, false, a));
        w.expect(0xf2, 3, {1, 101}, StatusCode::Ok, {1, 1, 101});
        w.expect(0xf2, 1, {1, 101}, StatusCode::DeviceNotFound, {});
        w.expect(0xf2, 3, {1, 101}, StatusCode::Ok, {1, 0, 101});
        // Catalogue accepts canonical references; existence is checked at mount.
        const std::string missing = "host:/disks/missing.adf";
        w.expect(0xf2, 2, put(222, false, missing), StatusCode::Ok, entry(222, false, missing));
        w.expect(0xf2, 1, {1, 222}, StatusCode::Ok, entry(222, false, missing));
        w.simple(6, 1);
        Bytes data(512, 0x73);
        auto write = sector_request(1);
        write.insert(write.end(), data.begin(), data.end());
        w.expect(0xfc, 4, write, StatusCode::Ok, sector_reply(1));
        w.info(1, geometry(1, 0x35, false, true));
        auto expected = seed(false, 0x40);
        std::copy(data.begin(), data.end(), expected.begin() + 3 * 512);
        const auto flushes = w.fs->flush_count();
        w.expect(0xfc, 1, mount(1, false, missing), StatusCode::InvalidRequest, {});
        // Existing semantics: old media survives, but dirty data is flushed and
        // lastError becomes FileNotFound (4); this is not transactional rollback.
        CHECK(w.fs->flush_count() == flushes + 1);
        w.info(1, geometry(1, 0x31, false, true, 4));
        w.read(1, expected); other_slot(w); w.backing(expected);
        w.expect(0xfc, 1, mount(1, false, "host:/disks/bad.adf"), StatusCode::InvalidRequest, {});
        w.info(1, geometry(1, 0x31, false, true, 8));
        w.read(1, expected); other_slot(w); w.backing(expected);
        w.expect(0xfc, 1, mount(9, false, a), StatusCode::InvalidRequest, {});
        write = sector_request(8);
        write.insert(write.end(), data.begin(), data.end());
        w.expect(0xfc, 4, write, StatusCode::InvalidRequest, {});
        other_slot(w); w.backing(expected);
        w.runtime("v1\n0\t0\trw\thost:/disks/a.adf\n7\t0\tr\thost:/disks/hd.adf\n");
        w.recreate();
        w.expect(0xf2, 1, {1, 101}, StatusCode::DeviceNotFound, {});
        w.expect(0xf2, 1, {1, 222}, StatusCode::Ok, entry(222, false, missing));
        CHECK(w.disk->restore_runtime_mounts() == std::vector<std::size_t>{0, 7});
        w.info(1, geometry(1, 0x39, false, true));
        w.info(8, geometry(8, 0x3b, true, true));
        w.simple(6, 8);
        w.read(1, expected); other_slot(w); w.backing(expected);
    }
}
TEST_CASE("Dirty independent writable slots and catalogue edits retain runtime media")
{
    for (bool native : {false, true}) {
        World w(native);
        populate(w);
        w.select(101, 8, false, b, false);
        auto expectedA = seed(false, 0x40);
        auto expectedB = seed(false, 0x61);
        auto write_sector = [&](std::uint8_t slot, std::uint8_t marker, Bytes& image) {
            Bytes data(512, marker);
            auto request = sector_request(slot);
            request.insert(request.end(), data.begin(), data.end());
            w.expect(0xfc, 4, request, StatusCode::Ok, sector_reply(slot));
            std::copy(data.begin(), data.end(), image.begin() + 3 * 512);
        };
        auto backing = [&]() {
            CHECK(w.fs->file_bytes("/disks/a.adf") == expectedA);
            CHECK(w.fs->file_bytes("/disks/b.adf") == expectedB);
            CHECK(w.fs->file_bytes("/disks/hd.adf") == seed(true, 0x82));
            CHECK(w.fs->file_bytes("/disks/bad.adf") == Bytes(513, 0));
        };
        auto untouched = [&]() {
            w.info(8, geometry(8, 0x3d, false, true));
            w.read(8, expectedB);
            backing();
        };
        write_sector(1, 0x22, expectedA);
        write_sector(8, 0x33, expectedB);
        w.info(1, geometry(1, 0x3d, false, true));
        untouched();
        auto flushes = w.fs->flush_count();
        w.simple(0x0e, 1);
        CHECK(w.fs->flush_count() == flushes + 1);
        w.info(1, geometry(1, 0x39, false, true));
        untouched();
        write_sector(1, 0x44, expectedA);
        flushes = w.fs->flush_count();
        w.select(200, 1, true, hd, true);
        // One outgoing image flush plus one persisted mapping flush.
        CHECK(w.fs->flush_count() == flushes + 2);
        w.info(1, geometry(1, 0x3b, true, true));
        untouched();
        w.select(100, 1, false, a, false);
        write_sector(1, 0x55, expectedA);
        flushes = w.fs->flush_count();
        w.simple(2, 1);
        CHECK(w.fs->flush_count() == flushes + 2);
        w.info(1, empty_info(1));
        untouched();
        w.runtime("v1\n7\t0\trw\thost:/disks/b.adf\n");
        // Catalogue overwrite changes URI and RO policy, without changing the
        // already active slot selected from this index.
        w.expect(0xf2, 2, put(101, true, hd), StatusCode::Ok, entry(101, true, hd));
        untouched();
        w.expect(0xf2, 3, {1, 100}, StatusCode::Ok, {1, 1, 100});
        w.select(101, 1, true, hd, true);
        w.expect(0xf2, 3, {1, 101}, StatusCode::Ok, {1, 1, 101});
        w.expect(0xf2, 1, {1, 101}, StatusCode::DeviceNotFound, {});
        w.info(1, geometry(1, 0x3b, true, true));
        w.read(1, seed(true, 0x82), 3519);
        untouched();
        w.runtime("v1\n0\t0\tr\thost:/disks/hd.adf\n7\t0\trw\thost:/disks/b.adf\n");
        // Keep a second overwritten entry so recreation proves URI and RO
        // persistence separately from the mounted-entry deletion.
        w.expect(0xf2, 2, put(200, false, a), StatusCode::Ok, entry(200, false, a));
        w.simple(0x0e, 8);
        w.recreate();
        w.expect(0xf2, 1, {1, 200}, StatusCode::Ok, entry(200, false, a));
        w.expect(0xf2, 1, {1, 101}, StatusCode::DeviceNotFound, {});
        CHECK(w.disk->restore_runtime_mounts() == std::vector<std::size_t>{0, 7});
        w.info(1, geometry(1, 0x3b, true, true));
        w.info(8, geometry(8, 0x39, false, true));
        w.read(1, seed(true, 0x82), 2000);
        w.read(8, expectedB);
        backing();
    }
}
} // TEST_SUITE
} // namespace
