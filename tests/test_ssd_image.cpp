#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/disk_types.h"
#include "fujinet/disk/ssd_image.h"
#include "fujinet/fs/storage_manager.h"

#include <cstring>
#include <memory>
#include <vector>

namespace {

using namespace fujinet::disk;
using namespace fujinet::fs;

// DFS sector count at file offsets $106 (high 2 bits) and $107 (low 8 bits).
constexpr std::size_t SSD_HEADER_SIZE = 0x108;

std::vector<std::uint8_t> make_ssd_header(std::uint32_t sectorCount)
{
    std::vector<std::uint8_t> buf(SSD_HEADER_SIZE, 0);
    buf[0x106] = static_cast<std::uint8_t>((sectorCount >> 8) & 0x03);
    buf[0x107] = static_cast<std::uint8_t>(sectorCount & 0xFF);
    return buf;
}

TEST_CASE("SSD image mount accepts full 800-sector image")
{
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->createDirectory("/");
    std::vector<std::uint8_t> bytes = make_ssd_header(800);
    bytes.resize(800 * 256);
    REQUIRE(memfs->create_file("/disk.ssd", bytes));

    fujinet::fs::FileInfo info;
    REQUIRE(memfs->stat("/disk.ssd", info));
    auto file = memfs->open("/disk.ssd", "rb");
    REQUIRE(file);

    auto img = make_ssd_disk_image();
    MountOptions opts{};
    auto r = img->mount(std::move(file), info.sizeBytes, opts);
    REQUIRE(r.ok());
    CHECK(img->geometry().sectorCount == 800);
    CHECK(img->geometry().sectorSize == 256);
}

TEST_CASE("SSD image mount accepts sparse image using header sector count")
{
    // Truncated file: only 28 KiB but header says 800 sectors (like a sparse SSD).
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->createDirectory("/");
    std::vector<std::uint8_t> bytes = make_ssd_header(800);
    bytes.resize(28 * 1024);  // 28k
    REQUIRE(memfs->create_file("/disk.ssd", bytes));

    fujinet::fs::FileInfo info;
    REQUIRE(memfs->stat("/disk.ssd", info));
    auto file = memfs->open("/disk.ssd", "r+b");
    REQUIRE(file);

    auto img = make_ssd_disk_image();
    MountOptions opts{};
    auto r = img->mount(std::move(file), info.sizeBytes, opts);
    REQUIRE(r.ok());
    CHECK(img->geometry().sectorCount == 800);
    CHECK(img->geometry().sectorSize == 256);
}

TEST_CASE("SSD image mount accepts 400-sector header")
{
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->createDirectory("/");
    std::vector<std::uint8_t> bytes = make_ssd_header(400);
    bytes.resize(512);  // minimal sparse
    REQUIRE(memfs->create_file("/disk.ssd", bytes));

    fujinet::fs::FileInfo info;
    REQUIRE(memfs->stat("/disk.ssd", info));
    auto file = memfs->open("/disk.ssd", "rb");
    REQUIRE(file);

    auto img = make_ssd_disk_image();
    MountOptions opts{};
    auto r = img->mount(std::move(file), info.sizeBytes, opts);
    REQUIRE(r.ok());
    CHECK(img->geometry().sectorCount == 400);
}

TEST_CASE("SSD image mount rejects file smaller than header")
{
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->createDirectory("/");
    std::vector<std::uint8_t> bytes(0x100, 0);  // 256 bytes, too small for $106/$107
    REQUIRE(memfs->create_file("/disk.ssd", bytes));

    fujinet::fs::FileInfo info;
    REQUIRE(memfs->stat("/disk.ssd", info));
    auto file = memfs->open("/disk.ssd", "rb");
    REQUIRE(file);

    auto img = make_ssd_disk_image();
    MountOptions opts{};
    auto r = img->mount(std::move(file), info.sizeBytes, opts);
    CHECK(!r.ok());
    CHECK(r.error == DiskError::BadImage);
}

TEST_CASE("SSD image mount rejects invalid sector count in header")
{
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->createDirectory("/");
    std::vector<std::uint8_t> bytes = make_ssd_header(999);  // not 400 or 800
    REQUIRE(memfs->create_file("/disk.ssd", bytes));

    fujinet::fs::FileInfo info;
    REQUIRE(memfs->stat("/disk.ssd", info));
    auto file = memfs->open("/disk.ssd", "rb");
    REQUIRE(file);

    auto img = make_ssd_disk_image();
    MountOptions opts{};
    auto r = img->mount(std::move(file), info.sizeBytes, opts);
    CHECK(!r.ok());
    CHECK(r.error == DiskError::BadImage);
}

TEST_CASE("SSD image sparse read returns zeros beyond file")
{
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->createDirectory("/");
    std::vector<std::uint8_t> bytes = make_ssd_header(800);
    bytes.resize(512);  // only 2 sectors present
    bytes[0] = 0xAA;
    bytes[256] = 0xBB;
    REQUIRE(memfs->create_file("/disk.ssd", bytes));

    fujinet::fs::FileInfo info;
    REQUIRE(memfs->stat("/disk.ssd", info));
    auto file = memfs->open("/disk.ssd", "rb");
    REQUIRE(file);

    auto img = make_ssd_disk_image();
    REQUIRE(img->mount(std::move(file), info.sizeBytes, MountOptions{}).ok());

    std::uint8_t sec[256];
    REQUIRE(img->read_sector(0, sec, sizeof(sec)).ok());
    CHECK(sec[0] == 0xAA);

    REQUIRE(img->read_sector(1, sec, sizeof(sec)).ok());
    CHECK(sec[0] == 0xBB);

    // Sector 2 is beyond file; should be all zeros.
    REQUIRE(img->read_sector(2, sec, sizeof(sec)).ok());
    for (std::size_t i = 0; i < 256; ++i) CHECK(sec[i] == 0);

    // Sector 799 is beyond file; should be all zeros.
    REQUIRE(img->read_sector(799, sec, sizeof(sec)).ok());
    for (std::size_t i = 0; i < 256; ++i) CHECK(sec[i] == 0);
}

TEST_CASE("SSD image sparse read returns partial data then zeros when sector straddles EOF")
{
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->createDirectory("/");
    std::vector<std::uint8_t> bytes = make_ssd_header(800);
    bytes.resize(256 + 64);  // sector 0 full, sector 1 only 64 bytes
    bytes[256] = 0x11;
    bytes[257] = 0x22;
    REQUIRE(memfs->create_file("/disk.ssd", bytes));

    fujinet::fs::FileInfo info;
    REQUIRE(memfs->stat("/disk.ssd", info));
    auto file = memfs->open("/disk.ssd", "rb");
    REQUIRE(file);

    auto img = make_ssd_disk_image();
    REQUIRE(img->mount(std::move(file), info.sizeBytes, MountOptions{}).ok());

    std::uint8_t sec[256];
    REQUIRE(img->read_sector(1, sec, sizeof(sec)).ok());
    CHECK(sec[0] == 0x11);
    CHECK(sec[1] == 0x22);
    for (std::size_t i = 64; i < 256; ++i) CHECK(sec[i] == 0);
}

TEST_CASE("SSD image sparse write extends file with zeros then writes sector")
{
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->createDirectory("/");
    std::vector<std::uint8_t> bytes = make_ssd_header(800);
    bytes.resize(512);  // 2 sectors
    REQUIRE(memfs->create_file("/disk.ssd", bytes));

    fujinet::fs::FileInfo info;
    REQUIRE(memfs->stat("/disk.ssd", info));
    auto file = memfs->open("/disk.ssd", "r+b");
    REQUIRE(file);

    auto img = make_ssd_disk_image();
    MountOptions opts{};
    opts.readOnlyRequested = false;
    REQUIRE(img->mount(std::move(file), info.sizeBytes, opts).ok());
    CHECK(!img->read_only());

    std::uint8_t sec[256];
    std::memset(sec, 0xDD, sizeof(sec));
    sec[0] = 0x99;
    sec[255] = 0x77;
    REQUIRE(img->write_sector(10, sec, sizeof(sec)).ok());

    REQUIRE(img->flush().ok());

    // Backing file should have been extended: 512 + gap (10*256 - 512) + 256 = 10*256 + 256 = 2816
    std::vector<std::uint8_t>& backing = memfs->file_bytes("/disk.ssd");
    REQUIRE(backing.size() >= 11 * 256);
    CHECK(backing[10 * 256 + 0] == 0x99);
    CHECK(backing[10 * 256 + 255] == 0x77);
    // Gap (bytes 512 .. 10*256-1) should be zeros
    for (std::size_t i = 512; i < 10 * 256; ++i) CHECK(backing[i] == 0);
}

TEST_CASE("DiskService mount sparse SSD via memory FS")
{
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->createDirectory("/");
    std::vector<std::uint8_t> bytes = make_ssd_header(800);
    bytes.resize(28 * 1024);
    REQUIRE(memfs->create_file("/disk.ssd", bytes));

    REQUIRE(sm.registerFileSystem(std::move(memfs)));

    DiskService svc(sm, make_default_image_registry());
    MountOptions opts{};
    opts.typeOverride = ImageType::Ssd;
    auto mr = svc.mount(0, "mem", "/disk.ssd", opts);
    REQUIRE(mr.ok());

    auto info = svc.info(0);
    CHECK(info.inserted);
    CHECK(info.geometry.sectorCount == 800);
    CHECK(info.geometry.sectorSize == 256);

    std::vector<std::uint8_t> sec(256);
    REQUIRE(svc.read_sector(0, 0, sec.data(), sec.size()).ok());
    REQUIRE(svc.read_sector(0, 500, sec.data(), sec.size()).ok());
    for (std::size_t i = 0; i < 256; ++i) CHECK(sec[i] == 0);

    REQUIRE(svc.unmount(0).ok());
}

} // namespace
