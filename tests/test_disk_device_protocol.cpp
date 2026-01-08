#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/disk_service.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/disk_codec.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"

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

    sec[0] = 0xAA;
    sec[1] = 0x55;
    REQUIRE(svc.write_sector(0, 0, sec.data(), sec.size()).ok());

    std::vector<std::uint8_t> sec2(256);
    REQUIRE(svc.read_sector(0, 0, sec2.data(), sec2.size()).ok());
    CHECK(sec2[0] == 0xAA);
    CHECK(sec2[1] == 0x55);

    CHECK(svc.info(0).dirty);
    REQUIRE(svc.unmount(0).ok());
    CHECK(!svc.info(0).inserted);
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
        diskproto::write_lp_u16_string(p, "mem");
        diskproto::write_lp_u16_string(p, path);

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
        diskproto::write_lp_u16_string(p, "mem");
        diskproto::write_lp_u16_string(p, "/created.img");

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
        diskproto::write_lp_u16_string(p, "mem");
        diskproto::write_lp_u16_string(p, "/created.img");

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


