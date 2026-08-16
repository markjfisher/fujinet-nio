#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/disk_service.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/disk_codec.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace diskproto = fujinet::io::diskproto;
using fujinet::io::DeviceID;
using fujinet::io::DiskDevice;
using fujinet::io::IORequest;
using fujinet::io::IOResponse;
using fujinet::io::StatusCode;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static constexpr std::uint8_t V = 1;

static std::vector<std::uint8_t> to_vec(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

static std::vector<std::uint8_t> make_fat12_floppy_bytes()
{
    std::vector<std::uint8_t> bytes(512 * 2880);

    bytes[0] = 0xeb;
    bytes[1] = 0x3c;
    bytes[2] = 0x90;
    const char oem[] = "mkfs.fat";
    std::memcpy(&bytes[3], oem, 8);
    bytes[11] = 0x00;
    bytes[12] = 0x02; // bytes per sector: 512
    bytes[13] = 0x01; // sectors per cluster
    bytes[14] = 0x01;
    bytes[15] = 0x00; // reserved sectors
    bytes[16] = 0x02; // FAT count
    bytes[17] = 0xe0;
    bytes[18] = 0x00; // root entries
    bytes[19] = 0x40;
    bytes[20] = 0x0b; // total sectors: 2880
    bytes[21] = 0xf0; // media descriptor
    bytes[22] = 0x09;
    bytes[23] = 0x00; // sectors per FAT
    bytes[24] = 0x12;
    bytes[25] = 0x00; // sectors per track
    bytes[26] = 0x02;
    bytes[27] = 0x00; // heads
    bytes[510] = 0x55;
    bytes[511] = 0xaa;

    return bytes;
}

static std::vector<std::uint8_t> make_ssd_bytes(std::uint32_t sectorCount = 800)
{
    std::vector<std::uint8_t> bytes(sectorCount * 256);
    bytes[0x106] = static_cast<std::uint8_t>((sectorCount >> 8) & 0x03);
    bytes[0x107] = static_cast<std::uint8_t>(sectorCount & 0xFF);
    return bytes;
}

static std::vector<std::uint8_t> make_adf_bytes()
{
    constexpr std::size_t sectorSize = 512;
    constexpr std::size_t sectorCount = 1760;
    std::vector<std::uint8_t> bytes(sectorSize * sectorCount);
    for (std::size_t sector = 0; sector < sectorCount; ++sector) {
        bytes[sector * sectorSize] = static_cast<std::uint8_t>(sector);
        bytes[sector * sectorSize + 1] = static_cast<std::uint8_t>(sector >> 8);
    }
    return bytes;
}

static std::vector<std::uint8_t> make_adf_bytes_with_boot(std::uint32_t sectors, std::uint8_t marker)
{
    auto bytes = make_adf_bytes();
    bytes.resize(static_cast<std::size_t>(sectors) * 512);
    bytes[0] = 'D'; bytes[1] = 'O'; bytes[2] = 'S'; bytes[3] = 0;
    bytes[4] = marker;
    return bytes;
}

TEST_CASE("DiskService: inspect ADF candidates without changing live slot")
{
    fujinet::fs::StorageManager sm;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    owned->file_bytes("/live.adf") = make_adf_bytes_with_boot(1760, 'A');
    owned->file_bytes("/dd.adf") = make_adf_bytes_with_boot(1760, 'D');
    owned->file_bytes("/hd.adf") = make_adf_bytes_with_boot(3520, 'H');
    REQUIRE(sm.registerFileSystem(std::move(owned)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    REQUIRE(svc.mount(0, "mem", "/live.adf", {}).ok());
    const auto before = svc.info(0);
    std::uint8_t liveBefore[512]{};
    REQUIRE(svc.read_sector(0, 0, liveBefore, sizeof(liveBefore)).ok());

    fujinet::disk::DiskMediaInspection dd{};
    REQUIRE(svc.inspect("mem", "/dd.adf", {}, 512, dd).ok());
    CHECK(dd.type == fujinet::disk::ImageType::Raw);
    CHECK(dd.geometry.sectorSize == 512);
    CHECK(dd.geometry.sectorCount == 1760);
    CHECK(dd.bootBytes.size() == 512);
    CHECK(std::memcmp(dd.bootBytes.data(), "DOS\0", 4) == 0);
    CHECK(dd.bootBytes[4] == 'D');

    fujinet::disk::DiskMediaInspection hd{};
    REQUIRE(svc.inspect("mem", "/hd.adf", {}, 512, hd).ok());
    CHECK(hd.geometry.sectorSize == 512);
    CHECK(hd.geometry.sectorCount == 3520);
    CHECK(hd.bootBytes[4] == 'H');
    CHECK(svc.inspect("mem", "/missing.adf", {}, 512, hd).error == fujinet::disk::DiskError::FileNotFound);

    const auto after = svc.info(0);
    CHECK(after.inserted == before.inserted);
    CHECK(after.readOnly == before.readOnly);
    CHECK(after.type == before.type);
    CHECK(after.geometry.sectorSize == before.geometry.sectorSize);
    CHECK(after.geometry.sectorCount == before.geometry.sectorCount);
    CHECK(after.changed == before.changed);
    std::uint8_t liveAfter[512]{};
    REQUIRE(svc.read_sector(0, 0, liveAfter, sizeof(liveAfter)).ok());
    CHECK(std::memcmp(liveBefore, liveAfter, sizeof(liveBefore)) == 0);
    CHECK(liveAfter[4] == 'A');
}

TEST_CASE("DiskDevice: Inspect returns candidate facts without changing live slot")
{
    fujinet::fs::StorageManager sm;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    owned->file_bytes("/live.adf") = make_adf_bytes_with_boot(1760, 'A');
    owned->file_bytes("/dd.adf") = make_adf_bytes_with_boot(1760, 'D');
    owned->file_bytes("/hd.adf") = make_adf_bytes_with_boot(3520, 'H');
    REQUIRE(sm.registerFileSystem(std::move(owned)));
    DiskDevice dev(sm);
    REQUIRE(dev.disk_service().mount(0, "mem", "/live.adf", {}).ok());
    const auto before = dev.disk_service().info(0);

    auto inspect = [&](const char* uri) {
        std::vector<std::uint8_t> payload;
        diskproto::write_u8(payload, V);
        diskproto::write_u8(payload, 0);
        diskproto::write_u8(payload, 0);
        diskproto::write_u16le(payload, 0);
        diskproto::write_u16le(payload, 512);
        diskproto::write_lp_u16_string(payload, uri);
        IORequest request{};
        request.id = 1;
        request.deviceId = to_device_id(WireDeviceId::DiskService);
        request.command = 0x0F;
        request.payload = std::move(payload);
        return dev.handle(request);
    };

    const auto dd = inspect("mem:/dd.adf");
    REQUIRE(dd.status == StatusCode::Ok);
    REQUIRE(dd.payload.size() == 522);
    CHECK(dd.payload[0] == V);
    CHECK(dd.payload[1] == static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw));
    CHECK(dd.payload[2] == 0x00); CHECK(dd.payload[3] == 0x02);
    CHECK(dd.payload[4] == 0xE0); CHECK(dd.payload[5] == 0x06);
    CHECK(std::memcmp(dd.payload.data() + 10, "DOS\0", 4) == 0);
    CHECK(dd.payload[14] == 'D');

    const auto hd = inspect("mem:/hd.adf");
    REQUIRE(hd.status == StatusCode::Ok);
    CHECK(hd.payload[4] == 0xC0); CHECK(hd.payload[5] == 0x0D);
    CHECK(hd.payload[14] == 'H');
    CHECK(inspect("mem:/missing.adf").status == StatusCode::InvalidRequest);

    const auto after = dev.disk_service().info(0);
    CHECK(after.inserted == before.inserted);
    CHECK(after.geometry.sectorSize == before.geometry.sectorSize);
    CHECK(after.geometry.sectorCount == before.geometry.sectorCount);
    CHECK(after.changed == before.changed);
    std::uint8_t live[512]{};
    REQUIRE(dev.disk_service().read_sector(0, 0, live, sizeof(live)).ok());
    CHECK(live[4] == 'A');
}

TEST_CASE("DiskService: ADF probes as raw 512-byte media")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->file_bytes("/disks/work.AdF") = make_adf_bytes();
    memfs->file_bytes("/disks/bad.ADF").resize(1760 * 512 - 1);
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    fujinet::disk::MountOptions opts{};
    REQUIRE(svc.mount(0, "mem", "/disks/work.AdF", opts).ok());

    const auto info = svc.info(0);
    CHECK(info.type == fujinet::disk::ImageType::Raw);
    CHECK(info.geometry.sectorSize == 512);
    CHECK(info.geometry.sectorCount == 1760);

    std::vector<std::uint8_t> sector(512);
    REQUIRE(svc.read_sector(0, 1759, sector.data(), sector.size()).ok());
    CHECK(sector[0] == 0xdf);
    CHECK(sector[1] == 0x06);

    std::fill(sector.begin(), sector.end(), 0xa5);
    REQUIRE(svc.write_sector(0, 0, sector.data(), sector.size()).ok());
    std::fill(sector.begin(), sector.end(), 0);
    REQUIRE(svc.read_sector(0, 0, sector.data(), sector.size()).ok());
    CHECK(sector[0] == 0xa5);

    CHECK(svc.read_sector(0, 1760, sector.data(), sector.size()).error == fujinet::disk::DiskError::OutOfRange);
    CHECK(svc.write_sector(0, 1760, sector.data(), sector.size()).error == fujinet::disk::DiskError::OutOfRange);
    CHECK(svc.write_sector(0, 0, sector.data(), 511).error == fujinet::disk::DiskError::InvalidSlot);

    REQUIRE(svc.unmount(0).ok());
    opts.readOnlyRequested = true;
    REQUIRE(svc.mount(0, "mem", "/disks/work.AdF", opts).ok());
    CHECK(svc.write_sector(0, 0, sector.data(), sector.size()).error == fujinet::disk::DiskError::ReadOnly);

    CHECK(svc.mount(1, "mem", "/disks/bad.ADF", fujinet::disk::MountOptions{}).error == fujinet::disk::DiskError::BadImage);
}

TEST_CASE("DiskService: raw ADF geometry overrides conflicting hints")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->file_bytes("/dd.adf") = make_adf_bytes_with_boot(1760, 'D');
    memfs->file_bytes("/hd.adf") = make_adf_bytes_with_boot(3520, 'H');
    memfs->file_bytes("/short.adf").resize(1760 * 512 - 1);
    memfs->file_bytes("/ambiguous.adf").resize(1680 * 512);
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    fujinet::disk::MountOptions conflictingHint{};
    conflictingHint.sectorSizeHint = 256;

    REQUIRE(svc.mount(0, "mem", "/dd.adf", conflictingHint).ok());
    CHECK(svc.info(0).geometry.sectorSize == 512);
    CHECK(svc.info(0).geometry.sectorCount == 1760);

    REQUIRE(svc.mount(1, "mem", "/hd.adf", conflictingHint).ok());
    CHECK(svc.info(1).geometry.sectorSize == 512);
    CHECK(svc.info(1).geometry.sectorCount == 3520);

    CHECK(svc.mount(2, "mem", "/short.adf", {}).error == fujinet::disk::DiskError::BadImage);
    CHECK(svc.mount(3, "mem", "/ambiguous.adf", {}).error == fujinet::disk::DiskError::BadImage);
}

TEST_CASE("DiskService: dirty and flush failure preserve mounted media")
{
    fujinet::fs::StorageManager sm;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* memfs = owned.get();
    memfs->file_bytes("/disk.adf") = make_adf_bytes();
    REQUIRE(sm.registerFileSystem(std::move(owned)));
    fujinet::disk::DiskService svc(
        sm, fujinet::disk::make_default_image_registry());
    REQUIRE(svc.mount(0, "mem", "/disk.adf", {}).ok());

    CHECK(svc.flush(0).ok());
    CHECK(memfs->flush_count() == 0);
    std::vector<std::uint8_t> sector(512, 0x5a);
    REQUIRE(svc.write_sector(0, 0, sector.data(), sector.size()).ok());
    CHECK(svc.info(0).dirty);

    memfs->set_fail_flush(true);
    CHECK(svc.flush(0).error == fujinet::disk::DiskError::IoError);
    CHECK(svc.info(0).dirty);
    CHECK(svc.info(0).lastError == fujinet::disk::DiskError::IoError);
    CHECK(svc.unmount(0).error == fujinet::disk::DiskError::IoError);
    CHECK(svc.info(0).inserted);

    memfs->set_fail_flush(false);
    REQUIRE(svc.flush(0).ok());
    CHECK_FALSE(svc.info(0).dirty);
    CHECK(svc.info(0).lastError == fujinet::disk::DiskError::None);
    const auto count = memfs->flush_count();
    REQUIRE(svc.flush(0).ok());
    CHECK(memfs->flush_count() == count);
}

TEST_CASE("DiskService: replacement is transactional around old-media flush")
{
    fujinet::fs::StorageManager sm;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* memfs = owned.get();
    memfs->file_bytes("/old.adf") = make_adf_bytes();
    memfs->file_bytes("/new.adf") = make_adf_bytes();
    REQUIRE(sm.registerFileSystem(std::move(owned)));
    fujinet::disk::DiskService svc(
        sm, fujinet::disk::make_default_image_registry());
    REQUIRE(svc.mount(0, "mem", "/old.adf", {}).ok());

    std::vector<std::uint8_t> sector(512, 0x37);
    REQUIRE(svc.write_sector(0, 0, sector.data(), sector.size()).ok());
    memfs->set_fail_flush(true);
    CHECK(svc.mount(0, "mem", "/new.adf", {}).error ==
          fujinet::disk::DiskError::IoError);
    CHECK(svc.info(0).inserted);
    CHECK(svc.info(0).dirty);

    memfs->set_fail_flush(false);
    CHECK(svc.mount(0, "mem", "/missing.adf", {}).error ==
          fujinet::disk::DiskError::FileNotFound);
    CHECK(svc.info(0).inserted);
    CHECK_FALSE(svc.info(0).dirty);
    CHECK(svc.info(0).path == "/old.adf");
}

TEST_CASE("DiskDevice v1: ADF uses the generic 512-byte wire contract")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->file_bytes("/disk.ADF") = make_adf_bytes();
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    const auto deviceId = to_device_id(WireDeviceId::DiskService);
    std::string mountPayload;
    diskproto::write_u8(mountPayload, V);
    diskproto::write_u8(mountPayload, 1);
    diskproto::write_u8(mountPayload, 0);
    diskproto::write_u8(mountPayload, 0); // automatic/raw by extension
    diskproto::write_u16le(mountPayload, 0);
    diskproto::write_lp_u16_string(mountPayload, "mem:/disk.ADF");

    IORequest mount{};
    mount.deviceId = deviceId;
    mount.command = 0x01;
    mount.payload = to_vec(mountPayload);
    const auto mounted = dev.handle(mount);
    REQUIRE(mounted.status == StatusCode::Ok);
    REQUIRE(mounted.payload.size() == 12);
    CHECK(mounted.payload[0] == V);
    CHECK(mounted.payload[1] == 1);
    CHECK(mounted.payload[4] == 1);
    CHECK(mounted.payload[5] == static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw));
    CHECK(mounted.payload[6] == 0x00);
    CHECK(mounted.payload[7] == 0x02);
    CHECK(mounted.payload[8] == 0xe0);
    CHECK(mounted.payload[9] == 0x06);
    CHECK(mounted.payload[10] == 0x00);
    CHECK(mounted.payload[11] == 0x00);

    std::string readPayload;
    diskproto::write_u8(readPayload, V);
    diskproto::write_u8(readPayload, 1);
    diskproto::write_u32le(readPayload, 1759);
    diskproto::write_u16le(readPayload, 512);
    IORequest read{};
    read.deviceId = deviceId;
    read.command = 0x03;
    read.payload = to_vec(readPayload);
    const auto readResponse = dev.handle(read);
    REQUIRE(readResponse.status == StatusCode::Ok);
    REQUIRE(readResponse.payload.size() == 523);
    CHECK(readResponse.payload[0] == V);
    CHECK(readResponse.payload[4] == 1);
    CHECK(readResponse.payload[5] == 0xdf);
    CHECK(readResponse.payload[6] == 0x06);

    std::string writePayload;
    diskproto::write_u8(writePayload, V);
    diskproto::write_u8(writePayload, 1);
    diskproto::write_u32le(writePayload, 0);
    diskproto::write_u16le(writePayload, 512);
    writePayload.append(512, static_cast<char>(0xa5));
    IORequest write{};
    write.deviceId = deviceId;
    write.command = 0x04;
    write.payload = to_vec(writePayload);
    const auto writeResponse = dev.handle(write);
    REQUIRE(writeResponse.status == StatusCode::Ok);
    REQUIRE(writeResponse.payload.size() == 11);
    CHECK(writeResponse.payload[0] == V);
    CHECK(writeResponse.payload[9] == 0x00);
    CHECK(writeResponse.payload[10] == 0x02);
}

TEST_CASE("DiskDevice v1: Flush 0x0E has the exact v1 slot vector")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->file_bytes("/disk.adf") = make_adf_bytes();
    REQUIRE(sm.registerFileSystem(std::move(memfs)));
    DiskDevice dev(sm);
    const auto deviceId = to_device_id(WireDeviceId::DiskService);

    std::string mountPayload;
    diskproto::write_u8(mountPayload, V);
    diskproto::write_u8(mountPayload, 1);
    diskproto::write_u8(mountPayload, 0);
    diskproto::write_u8(mountPayload, 0);
    diskproto::write_u16le(mountPayload, 512);
    diskproto::write_lp_u16_string(mountPayload, "mem:/disk.adf");
    IORequest mount{};
    mount.deviceId = deviceId;
    mount.command = 0x01;
    mount.payload = to_vec(mountPayload);
    REQUIRE(dev.handle(mount).status == StatusCode::Ok);

    IORequest flush{};
    flush.deviceId = deviceId;
    flush.command = 0x0E;
    flush.payload = {0x01, 0x01};
    const auto response = dev.handle(flush);
    REQUIRE(response.status == StatusCode::Ok);
    CHECK(response.payload == std::vector<std::uint8_t>{0x01, 0, 0, 0, 1});

    flush.payload.push_back(0);
    CHECK(dev.handle(flush).status == StatusCode::InvalidRequest);
}

TEST_CASE("DiskService: mount raw + read/write sector")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");

    // Create a 4-sector raw image (256 bytes per sector).
    const std::string path = "/disks/test.img";
    auto& bytes = memfs->file_bytes(path);
    bytes.resize(4 * 256);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i & 0xFF);

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());

    fujinet::disk::MountOptions opts{};
    opts.typeOverride = fujinet::disk::ImageType::Raw;
    opts.sectorSizeHint = 256;
    opts.readOnlyRequested = false;

    auto mr = svc.mount(0, "mem", path, opts);
    REQUIRE(mr.ok());

    auto info = svc.info(0);
    CHECK(info.inserted);
    CHECK(info.geometry.sectorSize == 256);
    CHECK(info.geometry.sectorCount == 4);
    CHECK(!info.readOnly);

    std::vector<std::uint8_t> sec(256);
    REQUIRE(svc.read_sector(0, 0, sec.data(), sec.size()).ok());
    CHECK(sec[0] == 0x00);
    CHECK(sec[1] == 0x01);

    REQUIRE(svc.read_sectors(0, 1, 2, sec.data(), sec.size()).error == fujinet::disk::DiskError::InvalidRequest);

    sec[0] = 0xAA;
    sec[1] = 0x55;
    REQUIRE(svc.write_sector(0, 0, sec.data(), sec.size()).ok());

    std::vector<std::uint8_t> sec2(256);
    REQUIRE(svc.read_sector(0, 0, sec2.data(), sec2.size()).ok());
    CHECK(sec2[0] == 0xAA);
    CHECK(sec2[1] == 0x55);

    auto stats = svc.stats(0);
    CHECK(stats.readRequests == 2);
    CHECK(stats.readSectors == 2);
    CHECK(stats.writeRequests == 1);
    CHECK(stats.writeSectors == 1);
    CHECK(stats.failedRequests == 1);
    CHECK(stats.image.readOps == 2);
    CHECK(stats.image.writeOps == 1);

    CHECK(svc.info(0).dirty);
    REQUIRE(svc.unmount(0).ok());
    CHECK(!svc.info(0).inserted);
}

TEST_CASE("DiskService: central probes detect FAT raw geometry without sector hint")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");

    const std::string path = "/disks/fn-dos.img";
    memfs->file_bytes(path) = make_fat12_floppy_bytes();

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());

    fujinet::disk::MountOptions opts{};
    opts.readOnlyRequested = true;

    auto mr = svc.mount(0, "mem", path, opts);
    REQUIRE(mr.ok());

    auto info = svc.info(0);
    CHECK(info.inserted);
    CHECK(info.type == fujinet::disk::ImageType::Raw);
    CHECK(info.geometry.sectorSize == 512);
    CHECK(info.geometry.sectorCount == 2880);
}

TEST_CASE("DiskService: pending raw mount uses sector size hint")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");

    const std::string path = "/disks/fn-dos.img";
    auto& bytes = memfs->file_bytes(path);
    bytes.resize(2 * 512);
    bytes[0] = 0xEB;
    bytes[1] = 0x3C;

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());

    svc.set_pending_mount(0, "mem:/disks/fn-dos.img", "rw", true, 512);

    auto pending = svc.get_pending_mount(0);
    REQUIRE(pending.has_value());
    CHECK(pending->sectorSizeHint == 512);

    std::vector<std::uint8_t> sec(512);
    auto rr = svc.read_sector(0, 0, sec.data(), sec.size());
    REQUIRE(rr.ok());
    CHECK(rr.bytes == 512);
    CHECK(sec[0] == 0xEB);
    CHECK(sec[1] == 0x3C);
    CHECK_FALSE(svc.get_pending_mount(0).has_value());

    auto info = svc.info(0);
    CHECK(info.inserted);
    CHECK(info.geometry.sectorSize == 512);
    CHECK(info.geometry.sectorCount == 2);
}

TEST_CASE("DiskService: pending mount replaces an active image on next access")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto& boot = memfs->file_bytes("/boot.ssd");
    auto& replacement = memfs->file_bytes("/replacement.ssd");
    boot = make_ssd_bytes();
    replacement = make_ssd_bytes();
    boot[0] = 'B';
    replacement[0] = 'R';
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    fujinet::disk::MountOptions opts{};
    REQUIRE(svc.mount(0, "mem", "/boot.ssd", opts).ok());

    std::vector<std::uint8_t> sector(256);
    REQUIRE(svc.read_sector(0, 0, sector.data(), sector.size()).ok());
    CHECK(sector[0] == 'B');

    svc.set_pending_mount(0, "mem:/replacement.ssd", "rw", true);
    CHECK_FALSE(svc.info(0).inserted);
    REQUIRE(svc.get_pending_mount(0).has_value());

    REQUIRE(svc.read_sector(0, 0, sector.data(), sector.size()).ok());
    CHECK(sector[0] == 'R');
    CHECK_FALSE(svc.get_pending_mount(0).has_value());
    CHECK(svc.info(0).inserted);
    CHECK(svc.info(0).path == "/replacement.ssd");
}

TEST_CASE("DiskService: failed replacement preserves the active image")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto& oldBytes = memfs->file_bytes("/old.ssd");
    oldBytes = make_ssd_bytes();
    oldBytes[0] = 'O';
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    fujinet::disk::MountOptions opts{};
    REQUIRE(svc.mount(0, "mem", "/old.ssd", opts).ok());

    auto failed = svc.mount(0, "mem", "/missing.ssd", opts);
    CHECK(failed.error == fujinet::disk::DiskError::FileNotFound);
    auto info = svc.info(0);
    CHECK(info.inserted);
    CHECK(info.path == "/old.ssd");

    std::vector<std::uint8_t> sector(256);
    REQUIRE(svc.read_sector(0, 0, sector.data(), sector.size()).ok());
    CHECK(sector[0] == 'O');
}

TEST_CASE("DiskService: unmount cancels a pending lazy mount")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->file_bytes("/lazy.ssd") = make_ssd_bytes();
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    svc.set_pending_mount(0, "mem:/lazy.ssd", "rw", true);
    REQUIRE(svc.get_pending_mount(0).has_value());

    REQUIRE(svc.unmount(0).ok());
    CHECK_FALSE(svc.get_pending_mount(0).has_value());
    CHECK(svc.ensure_mounted(0).error == fujinet::disk::DiskError::NotMounted);
}

TEST_CASE("DiskDevice v1: ReadSector activates pending raw mount")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");

    const std::string path = "/disks/fn-dos.img";
    auto& bytes = memfs->file_bytes(path);
    bytes.resize(2 * 512);
    bytes[0] = 0xEB;
    bytes[1] = 0x3C;

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    dev.disk_service().set_pending_mount(
        0, "mem:/disks/fn-dos.img", "rw", true, 512);
    CHECK_FALSE(dev.disk_service().info(0).inserted);

    std::string p;
    diskproto::write_u8(p, V);
    diskproto::write_u8(p, 1);
    diskproto::write_u32le(p, 0);
    diskproto::write_u16le(p, 512);

    IORequest req{};
    req.id = 3;
    req.deviceId = deviceId;
    req.command = 0x03; // ReadSector
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);

    diskproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0, flags = 0, slot = 0;
    std::uint16_t reserved = 0, dataLen = 0;
    std::uint32_t lba = 0;
    const std::uint8_t* data = nullptr;

    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(flags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u8(slot));
    REQUIRE(r.read_u32le(lba));
    REQUIRE(r.read_u16le(dataLen));
    REQUIRE(r.read_bytes(data, dataLen));

    CHECK(ver == V);
    CHECK(flags == 0);
    CHECK(slot == 1);
    CHECK(lba == 0);
    CHECK(dataLen == 512);
    CHECK(data[0] == 0xEB);
    CHECK(data[1] == 0x3C);

    auto info = dev.disk_service().info(0);
    CHECK(info.inserted);
    CHECK(info.geometry.sectorSize == 512);
    CHECK(info.geometry.sectorCount == 2);
}

TEST_CASE("DiskDevice v1: Info activates pending mount and reports geometry")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");

    const std::string path = "/disks/fn-boot.img";
    memfs->file_bytes(path) = make_fat12_floppy_bytes();

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    dev.disk_service().set_pending_mount(
        0, "mem:/disks/fn-boot.img", "r", true, 0);
    CHECK_FALSE(dev.disk_service().info(0).inserted);

    std::string p;
    diskproto::write_u8(p, V);
    diskproto::write_u8(p, 1);

    IORequest req{};
    req.id = 4;
    req.deviceId = deviceId;
    req.command = 0x05; // Info
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);

    diskproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0, flags = 0, slot = 0, type = 0, lastErr = 0;
    std::uint16_t reserved = 0, sectorSize = 0;
    std::uint32_t sectorCount = 0;

    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(flags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u8(slot));
    REQUIRE(r.read_u8(type));
    REQUIRE(r.read_u16le(sectorSize));
    REQUIRE(r.read_u32le(sectorCount));
    REQUIRE(r.read_u8(lastErr));

    CHECK(ver == V);
    CHECK((flags & 0x01) != 0); // inserted
    CHECK((flags & 0x02) != 0); // read-only
    CHECK((flags & 0x10) != 0); // has geometry
    CHECK(slot == 1);
    CHECK(type == static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw));
    CHECK(sectorSize == 512);
    CHECK(sectorCount == 2880);
    CHECK(lastErr == 0);

    auto info = dev.disk_service().info(0);
    CHECK(info.inserted);
    CHECK(info.geometry.sectorSize == 512);
    CHECK(info.geometry.sectorCount == 2880);
}

TEST_CASE("DiskDevice v1: RestoreBoot mounts configured boot image over existing slot")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");

    const std::string oldPath = "/disks/old.img";
    auto& oldBytes = memfs->file_bytes(oldPath);
    oldBytes.resize(2 * 256);
    oldBytes[0] = 0x11;

    const std::string bootPath = "/boot/autorun.img";
    memfs->file_bytes(bootPath) = make_fat12_floppy_bytes();

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    {
        fujinet::disk::MountOptions opts{};
        opts.typeOverride = fujinet::disk::ImageType::Raw;
        opts.sectorSizeHint = 256;
        REQUIRE(dev.disk_service().mount(0, "mem", oldPath, opts).ok());
        CHECK(dev.disk_service().info(0).geometry.sectorSize == 256);
    }

    dev.configure_boot_mount("mem:/boot/autorun.img", true);

    std::string p;
    diskproto::write_u8(p, V);
    diskproto::write_u8(p, 1);

    IORequest req{};
    req.id = 5;
    req.deviceId = deviceId;
    req.command = 0x0A; // RestoreBoot
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);

    diskproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0, flags = 0, slot = 0, type = 0;
    std::uint16_t reserved = 0, sectorSize = 0;
    std::uint32_t sectorCount = 0;

    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(flags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u8(slot));
    REQUIRE(r.read_u8(type));
    REQUIRE(r.read_u16le(sectorSize));
    REQUIRE(r.read_u32le(sectorCount));

    CHECK(ver == V);
    CHECK((flags & 0x01) != 0);
    CHECK((flags & 0x02) != 0);
    CHECK(slot == 1);
    CHECK(type == static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw));
    CHECK(sectorSize == 512);
    CHECK(sectorCount == 2880);

    auto info = dev.disk_service().info(0);
    CHECK(info.inserted);
    CHECK(info.readOnly);
    CHECK(info.changed);
    CHECK(info.geometry.sectorSize == 512);
    CHECK(info.geometry.sectorCount == 2880);
}

TEST_CASE("DiskDevice v1: RestoreBoot without configured boot image is NotReady")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    std::string p;
    diskproto::write_u8(p, V);
    diskproto::write_u8(p, 1);

    IORequest req{};
    req.id = 6;
    req.deviceId = deviceId;
    req.command = 0x0A; // RestoreBoot
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    CHECK(resp.status == StatusCode::NotReady);
}

TEST_CASE("DiskDevice v1: mounted runtime state restores as pending in a new device instance")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");

    const std::string path = "/disks/work.img";
    auto& bytes = memfs->file_bytes(path);
    bytes.resize(2 * 512);
    bytes[0] = 0xEB;
    bytes[1] = 0x3C;

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    {
        DiskDevice dev(sm);

        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1);
        diskproto::write_u8(p, 0);
        diskproto::write_u8(p, static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw));
        diskproto::write_u16le(p, 512);
        diskproto::write_lp_u16_string(p, "host:/disks/work.img");

        IORequest req{};
        req.id = 7;
        req.deviceId = deviceId;
        req.command = 0x01; // Mount
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);
    }

    DiskDevice restored(sm);
    auto restoredSlots = restored.restore_runtime_mounts();
    REQUIRE(restoredSlots.size() == 1);
    CHECK(restoredSlots[0] == 0);
    CHECK_FALSE(restored.disk_service().info(0).inserted);

    std::string p;
    diskproto::write_u8(p, V);
    diskproto::write_u8(p, 1);

    IORequest req{};
    req.id = 8;
    req.deviceId = deviceId;
    req.command = 0x05; // Info
    req.payload = to_vec(p);

    IOResponse resp = restored.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    CHECK(restored.disk_service().info(0).inserted);
    CHECK(restored.disk_service().info(0).geometry.sectorSize == 512);
}

TEST_CASE("DiskDevice v1: ListMounts reports only active runtime mappings")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->file_bytes("/chuck.ssd") = make_ssd_bytes();
    memfs->file_bytes("/bwc.ssd") = make_ssd_bytes();
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    auto list_mounts = [&]() {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 0x01); // formatted
        diskproto::write_u16le(p, 0);
        diskproto::write_u16le(p, 0);
        diskproto::write_u16le(p, 0);
        diskproto::write_u16le(p, 220);

        IORequest req{};
        req.id = 30;
        req.deviceId = deviceId;
        req.command = 0x0D; // ListMounts
        req.payload = to_vec(p);
        return dev.handle(req);
    };

    const auto empty = list_mounts();
    REQUIRE(empty.status == StatusCode::Ok);
    REQUIRE(empty.payload.size() == 10);
    CHECK(empty.payload[0] == V);
    CHECK(empty.payload[1] == 0x02);
    CHECK(empty.payload[6] == 0);
    CHECK(empty.payload[8] == 0);

    auto mount = [&](std::uint8_t slot, const char* uri) {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, slot);
        diskproto::write_u8(p, 0);
        diskproto::write_u8(
            p, static_cast<std::uint8_t>(fujinet::disk::ImageType::Ssd));
        diskproto::write_u16le(p, 0);
        diskproto::write_lp_u16_string(p, uri);

        IORequest req{};
        req.id = slot;
        req.deviceId = deviceId;
        req.command = 0x01;
        req.payload = to_vec(p);
        return dev.handle(req);
    };

    REQUIRE(mount(1, "mem:/chuck.ssd").status == StatusCode::Ok);
    REQUIRE(mount(2, "mem:/bwc.ssd").status == StatusCode::Ok);

    const auto listed = list_mounts();
    REQUIRE(listed.status == StatusCode::Ok);
    REQUIRE(listed.payload.size() >= 10);
    CHECK(listed.payload[0] == V);
    CHECK(listed.payload[1] == 0x02);
    CHECK(listed.payload[6] == 2);
    const std::string text(listed.payload.begin() + 10, listed.payload.end());
    CHECK(text == "0: AUTO mem:/chuck.ssd\n1: AUTO mem:/bwc.ssd\n");
}

TEST_CASE("DiskDevice v1: lazy mount flag stages URI without opening the image")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    std::string p;
    diskproto::write_u8(p, V);
    diskproto::write_u8(p, 8);
    diskproto::write_u8(p, 0x03); // read-only + lazy
    diskproto::write_u8(p, 0);
    diskproto::write_u16le(p, 0);
    diskproto::write_lp_u16_string(p, "host:/images/not-opened-yet.ssd");

    IORequest req{};
    req.deviceId = to_device_id(WireDeviceId::DiskService);
    req.command = 0x01;
    req.payload = to_vec(p);

    const IOResponse resp = dev.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    CHECK_FALSE(dev.disk_service().info(7).inserted);
    const auto pending = dev.disk_service().get_pending_mount(7);
    REQUIRE(pending.has_value());
    CHECK(pending->uri == "host:/images/not-opened-yet.ssd");
    CHECK(pending->mode == "r");

    std::string list;
    diskproto::write_u8(list, V);
    diskproto::write_u8(list, 0x01); // formatted
    diskproto::write_u16le(list, 0);
    diskproto::write_u16le(list, 0);
    diskproto::write_u16le(list, 0);
    diskproto::write_u16le(list, 220);
    IORequest listReq{};
    listReq.deviceId = to_device_id(WireDeviceId::DiskService);
    listReq.command = 0x0D; // ListMounts
    listReq.payload = to_vec(list);
    const IOResponse listed = dev.handle(listReq);
    REQUIRE(listed.status == StatusCode::Ok);
    REQUIRE(listed.payload.size() >= 10);
    CHECK(listed.payload[6] == 1);
    CHECK(std::string(listed.payload.begin() + 10, listed.payload.end()) ==
          "7: RO host:/images/not-opened-yet.ssd\n");
}

TEST_CASE("DiskDevice v1: BeginHostSession clears runtime state and restores boot disk")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");

    auto& work = memfs->file_bytes("/disks/work.img");
    work.resize(2 * 512);
    work[0] = 0x11;

    memfs->file_bytes("/boot/autorun.img") = make_fat12_floppy_bytes();

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    DiskDevice dev(sm);
    dev.configure_boot_mount("host:/boot/autorun.img", true);

    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1);
        diskproto::write_u8(p, 0);
        diskproto::write_u8(p, static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw));
        diskproto::write_u16le(p, 512);
        diskproto::write_lp_u16_string(p, "host:/disks/work.img");

        IORequest req{};
        req.id = 9;
        req.deviceId = deviceId;
        req.command = 0x01; // Mount
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);
    }

    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1);

        IORequest req{};
        req.id = 10;
        req.deviceId = deviceId;
        req.command = 0x0B; // BeginHostSession
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);

        auto info = dev.disk_service().info(0);
        CHECK(info.inserted);
        CHECK(info.readOnly);
        CHECK(info.geometry.sectorSize == 512);
        CHECK(info.geometry.sectorCount == 2880);
    }

    DiskDevice afterSession(sm);
    CHECK(afterSession.restore_runtime_mounts().empty());
}

TEST_CASE("DiskDevice v1: BeginHostSession restores configured BBC SSD boot disk")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");

    memfs->file_bytes("/boot/bbc/FN-BOOT.ssd") = make_ssd_bytes();

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    DiskDevice dev(sm);
    dev.configure_boot_mount("host:/boot/bbc/FN-BOOT.ssd", true);

    std::string p;
    diskproto::write_u8(p, V);
    diskproto::write_u8(p, 1);

    IORequest req{};
    req.id = 11;
    req.deviceId = deviceId;
    req.command = 0x0B; // BeginHostSession
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);

    diskproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0, flags = 0, slot = 0, type = 0;
    std::uint16_t reserved = 0, sectorSize = 0;
    std::uint32_t sectorCount = 0;

    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(flags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u8(slot));
    REQUIRE(r.read_u8(type));
    REQUIRE(r.read_u16le(sectorSize));
    REQUIRE(r.read_u32le(sectorCount));

    CHECK(ver == V);
    CHECK((flags & 0x01) != 0); // mounted
    CHECK((flags & 0x02) != 0); // read-only
    CHECK(slot == 1);
    CHECK(type == static_cast<std::uint8_t>(fujinet::disk::ImageType::Ssd));
    CHECK(sectorSize == 256);
    CHECK(sectorCount == 800);
}

TEST_CASE("DiskDevice v1: Mount -> Info -> ReadSector -> WriteSector -> Close")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");

    // Create a 2-sector raw image.
    const std::string path = "/disks/test.img";
    auto& bytes = memfs->file_bytes(path);
    bytes.resize(2 * 256);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = 0;
    bytes[0] = 0x11;
    bytes[1] = 0x22;

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    // ---- Mount ----
    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1); // slot D1
        diskproto::write_u8(p, 0); // flags (rw requested)
        diskproto::write_u8(p, static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw)); // override raw
        diskproto::write_u16le(p, 256); // sectorSizeHint
        diskproto::write_lp_u16_string(p, "mem://" + path);

        IORequest req{};
        req.id = 1;
        req.deviceId = deviceId;
        req.command = 0x01; // Mount
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);

        diskproto::Reader r(resp.payload.data(), resp.payload.size());
        std::uint8_t ver = 0, flags = 0, slot = 0, type = 0;
        std::uint16_t reserved = 0, sectorSize = 0;
        std::uint32_t sectorCount = 0;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u8(slot));
        REQUIRE(r.read_u8(type));
        REQUIRE(r.read_u16le(sectorSize));
        REQUIRE(r.read_u32le(sectorCount));

        CHECK(ver == V);
        CHECK(slot == 1);
        CHECK((flags & 0x01) != 0); // mounted
        CHECK(sectorSize == 256);
        CHECK(sectorCount == 2);
    }

    // ---- Info ----
    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1); // slot

        IORequest req{};
        req.id = 2;
        req.deviceId = deviceId;
        req.command = 0x05; // Info
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);

        diskproto::Reader r(resp.payload.data(), resp.payload.size());
        std::uint8_t ver = 0, flags = 0, slot = 0, type = 0, lastErr = 0;
        std::uint16_t reserved = 0, sectorSize = 0;
        std::uint32_t sectorCount = 0;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u8(slot));
        REQUIRE(r.read_u8(type));
        REQUIRE(r.read_u16le(sectorSize));
        REQUIRE(r.read_u32le(sectorCount));
        REQUIRE(r.read_u8(lastErr));

        CHECK(ver == V);
        CHECK((flags & 0x01) != 0); // inserted
        CHECK(sectorSize == 256);
        CHECK(sectorCount == 2);
        CHECK(lastErr == 0);
    }

    // ---- ReadSector ----
    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1); // slot
        diskproto::write_u32le(p, 0); // lba
        diskproto::write_u16le(p, 256);

        IORequest req{};
        req.id = 3;
        req.deviceId = deviceId;
        req.command = 0x03; // ReadSector
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);

        diskproto::Reader r(resp.payload.data(), resp.payload.size());
        std::uint8_t ver = 0, flags = 0, slot = 0;
        std::uint16_t reserved = 0, dataLen = 0;
        std::uint32_t lba = 0;
        const std::uint8_t* bytes = nullptr;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u8(slot));
        REQUIRE(r.read_u32le(lba));
        REQUIRE(r.read_u16le(dataLen));
        REQUIRE(r.read_bytes(bytes, dataLen));

        CHECK(ver == V);
        CHECK(slot == 1);
        CHECK(lba == 0);
        CHECK(flags == 0);
        CHECK(dataLen == 256);
        CHECK(bytes[0] == 0x11);
        CHECK(bytes[1] == 0x22);
    }

    // ---- WriteSector ----
    {
        std::vector<std::uint8_t> sec(256, 0);
        sec[0] = 0xAA;
        sec[1] = 0x55;

        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1);
        diskproto::write_u32le(p, 0);
        diskproto::write_u16le(p, static_cast<std::uint16_t>(sec.size()));
        p.append(reinterpret_cast<const char*>(sec.data()), sec.size());

        IORequest req{};
        req.id = 4;
        req.deviceId = deviceId;
        req.command = 0x04; // WriteSector
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);
    }

    // ---- Read back ----
    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1);
        diskproto::write_u32le(p, 0);
        diskproto::write_u16le(p, 256);

        IORequest req{};
        req.id = 5;
        req.deviceId = deviceId;
        req.command = 0x03; // ReadSector
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);

        diskproto::Reader r(resp.payload.data(), resp.payload.size());
        std::uint8_t ver = 0, flags = 0, slot = 0;
        std::uint16_t reserved = 0, dataLen = 0;
        std::uint32_t lba = 0;
        const std::uint8_t* bytes = nullptr;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u8(slot));
        REQUIRE(r.read_u32le(lba));
        REQUIRE(r.read_u16le(dataLen));
        REQUIRE(r.read_bytes(bytes, dataLen));

        CHECK(bytes[0] == 0xAA);
        CHECK(bytes[1] == 0x55);
    }

    // ---- Unmount ----
    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1);

        IORequest req{};
        req.id = 6;
        req.deviceId = deviceId;
        req.command = 0x02; // Unmount
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);
    }
}

TEST_CASE("DiskDevice v1: Create raw image then mount and read/write")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    const DeviceID deviceId = to_device_id(WireDeviceId::DiskService);

    // ---- Create ----
    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1); // flags: overwrite
        diskproto::write_u8(p, static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw));
        diskproto::write_u16le(p, 256);
        diskproto::write_u32le(p, 4);
        diskproto::write_lp_u16_string(p, "mem:///created.img");

        IORequest req{};
        req.id = 10;
        req.deviceId = deviceId;
        req.command = 0x07; // Create
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);
    }

    // ---- Mount ----
    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1); // slot D1
        diskproto::write_u8(p, 0); // rw
        diskproto::write_u8(p, static_cast<std::uint8_t>(fujinet::disk::ImageType::Raw));
        diskproto::write_u16le(p, 256);
        diskproto::write_lp_u16_string(p, "mem:///created.img");

        IORequest req{};
        req.id = 11;
        req.deviceId = deviceId;
        req.command = 0x01; // Mount
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);
    }

    // ---- WriteSector lba=0 ----
    {
        std::vector<std::uint8_t> sec(256, 0);
        sec[0] = 0xAA;
        sec[1] = 0x55;

        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1);
        diskproto::write_u32le(p, 0);
        diskproto::write_u16le(p, static_cast<std::uint16_t>(sec.size()));
        p.append(reinterpret_cast<const char*>(sec.data()), sec.size());

        IORequest req{};
        req.id = 12;
        req.deviceId = deviceId;
        req.command = 0x04; // WriteSector
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);
    }

    // ---- ReadSector lba=0 ----
    {
        std::string p;
        diskproto::write_u8(p, V);
        diskproto::write_u8(p, 1);
        diskproto::write_u32le(p, 0);
        diskproto::write_u16le(p, 256);

        IORequest req{};
        req.id = 13;
        req.deviceId = deviceId;
        req.command = 0x03; // ReadSector
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);

        diskproto::Reader r(resp.payload.data(), resp.payload.size());
        std::uint8_t ver = 0, flags = 0, slot = 0;
        std::uint16_t reserved = 0, dataLen = 0;
        std::uint32_t lba = 0;
        const std::uint8_t* bytes = nullptr;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u8(slot));
        REQUIRE(r.read_u32le(lba));
        REQUIRE(r.read_u16le(dataLen));
        REQUIRE(r.read_bytes(bytes, dataLen));

        CHECK(dataLen == 256);
        CHECK(bytes[0] == 0xAA);
        CHECK(bytes[1] == 0x55);
    }
}

TEST_CASE("DiskDevice v1: Reinitialize recreates and remounts the active SSD")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* memfsPtr = memfs.get();
    auto& original = memfs->file_bytes("/games/work.ssd");
    original = make_ssd_bytes(800);
    std::memcpy(original.data(), "OLD-DISK", 8);
    original[512] = 0xA5;
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskDevice dev(sm);
    fujinet::disk::MountOptions opts{};
    opts.typeOverride = fujinet::disk::ImageType::Ssd;
    REQUIRE(dev.disk_service().mount(0, "mem", "/games/work.ssd", opts).ok());

    std::string p;
    diskproto::write_u8(p, V);
    diskproto::write_u8(p, 1);
    diskproto::write_u16le(p, 256);
    diskproto::write_u32le(p, 400);

    IORequest req{};
    req.id = 20;
    req.deviceId = to_device_id(WireDeviceId::DiskService);
    req.command = 0x0C; // Reinitialize
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);

    diskproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0, flags = 0, slot = 0, type = 0;
    std::uint16_t reserved = 0, sectorSize = 0;
    std::uint32_t sectorCount = 0;
    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(flags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u8(slot));
    REQUIRE(r.read_u8(type));
    REQUIRE(r.read_u16le(sectorSize));
    REQUIRE(r.read_u32le(sectorCount));
    CHECK(ver == V);
    CHECK((flags & 0x01) != 0);
    CHECK(slot == 1);
    CHECK(type == static_cast<std::uint8_t>(fujinet::disk::ImageType::Ssd));
    CHECK(sectorSize == 256);
    CHECK(sectorCount == 400);

    const auto info = dev.disk_service().info(0);
    CHECK(info.inserted);
    CHECK(info.geometry.sectorCount == 400);
    const auto& recreated = memfsPtr->file_bytes("/games/work.ssd");
    REQUIRE(recreated.size() == 400 * 256);
    CHECK(std::memcmp(recreated.data(), "BLANK", 5) == 0);
    CHECK(recreated[512] == 0);
}

TEST_CASE("DiskService: Reinitialize rejects read-only media without changing it")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* memfsPtr = memfs.get();
    memfs->file_bytes("/readonly.ssd") = make_ssd_bytes(800);
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    fujinet::disk::MountOptions opts{};
    opts.typeOverride = fujinet::disk::ImageType::Ssd;
    opts.readOnlyRequested = true;
    REQUIRE(svc.mount(0, "mem", "/readonly.ssd", opts).ok());

    const auto before = memfsPtr->file_bytes("/readonly.ssd");
    CHECK(svc.reinitialize(0, 256, 400).error == fujinet::disk::DiskError::ReadOnly);
    CHECK(memfsPtr->file_bytes("/readonly.ssd") == before);
    CHECK(svc.info(0).inserted);
    CHECK(svc.info(0).geometry.sectorCount == 800);
}

TEST_CASE("DiskService: Reinitialize rejects invalid geometry without unmounting media")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* memfsPtr = memfs.get();
    memfs->file_bytes("/work.ssd") = make_ssd_bytes(800);
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    fujinet::disk::MountOptions opts{};
    opts.typeOverride = fujinet::disk::ImageType::Ssd;
    REQUIRE(svc.mount(0, "mem", "/work.ssd", opts).ok());

    const auto before = memfsPtr->file_bytes("/work.ssd");
    CHECK(svc.reinitialize(0, 256, 123).error == fujinet::disk::DiskError::InvalidGeometry);
    CHECK(memfsPtr->file_bytes("/work.ssd") == before);
    CHECK(svc.info(0).inserted);
    CHECK(svc.info(0).geometry.sectorCount == 800);
}

TEST_CASE("DiskService: unsupported creator is rejected before an existing file is truncated")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* memfsPtr = memfs.get();
    memfs->file_bytes("/keep.dsd") = {1, 2, 3, 4};
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    const auto result = svc.create_image(
        "mem", "/keep.dsd", fujinet::disk::ImageType::Dsd, 256, 800, true);
    CHECK(result.error == fujinet::disk::DiskError::UnsupportedImageType);
    CHECK(memfsPtr->file_bytes("/keep.dsd") == std::vector<std::uint8_t>{1, 2, 3, 4});
}

TEST_CASE("DiskService: invalid creator geometry is rejected before an existing file is truncated")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* memfsPtr = memfs.get();
    memfs->file_bytes("/keep.ssd") = make_ssd_bytes(800);
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());
    const auto before = memfsPtr->file_bytes("/keep.ssd");
    const auto result = svc.create_image(
        "mem", "/keep.ssd", fujinet::disk::ImageType::Ssd, 256, 123, true);
    CHECK(result.error == fujinet::disk::DiskError::InvalidGeometry);
    CHECK(memfsPtr->file_bytes("/keep.ssd") == before);
}

TEST_CASE("AtrDiskImage: 256-byte ATR has 128-byte first three sectors")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    fujinet::disk::DiskService svc(sm, fujinet::disk::make_default_image_registry());

    // Create a small ATR: 256-byte logical sectors, 10 sectors total.
    // (First 3 are 128 bytes by ATR convention)
    auto cr = svc.create_image("mem", "/t.atr", fujinet::disk::ImageType::Atr, 256, 10, true);
    REQUIRE(cr.ok());

    fujinet::disk::MountOptions mo{};
    mo.typeOverride = fujinet::disk::ImageType::Atr;
    auto mr = svc.mount(0, "mem", "/t.atr", mo);
    REQUIRE(mr.ok());

    auto info = svc.info(0);
    CHECK(info.type == fujinet::disk::ImageType::Atr);
    CHECK(info.geometry.sectorSize == 256);
    CHECK(info.geometry.sectorCount == 10);

    std::vector<std::uint8_t> buf(256);

    // Write sector 1 (lba=0) with 128 bytes.
    std::vector<std::uint8_t> s1(128, 0x11);
    auto wr1 = svc.write_sector(0, 0, s1.data(), s1.size());
    REQUIRE(wr1.ok());
    CHECK(wr1.bytes == 128);

    auto rr1 = svc.read_sector(0, 0, buf.data(), buf.size());
    REQUIRE(rr1.ok());
    CHECK(rr1.bytes == 128);
    CHECK(buf[0] == 0x11);

    // Write sector 4 (lba=3) with 256 bytes.
    std::vector<std::uint8_t> s4(256, 0x22);
    auto wr4 = svc.write_sector(0, 3, s4.data(), s4.size());
    REQUIRE(wr4.ok());
    CHECK(wr4.bytes == 256);

    auto rr4 = svc.read_sector(0, 3, buf.data(), buf.size());
    REQUIRE(rr4.ok());
    CHECK(rr4.bytes == 256);
    CHECK(buf[0] == 0x22);
}
