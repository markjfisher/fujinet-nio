#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/cbm_volume.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/disk/prg_image.h"
#include "fujinet/fs/storage_manager.h"

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace fujinet::disk;

namespace {

constexpr std::uint32_t disk_sectors = 683;
constexpr std::uint32_t data_sectors = 664;      // all but the 19 on track 18
constexpr std::size_t bytes_per_data_sector = 254;
constexpr std::size_t sectors_before_track_18 = 17 * 21;
constexpr std::size_t largest_file = data_sectors * bytes_per_data_sector;

// A program of `length` bytes, every position distinct enough to catch a slip.
std::vector<std::uint8_t> program(std::size_t length)
{
    std::vector<std::uint8_t> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 31 + (i >> 8) * 7 + 1);
    }
    if (length >= 2) {
        bytes[0] = 0x01;
        bytes[1] = 0x08;
    }
    return bytes;
}

struct Rig {
    fujinet::fs::StorageManager storage;
    fujinet::tests::MemoryFileSystem* fs{nullptr};
    std::unique_ptr<DiskService> disks;

    Rig()
    {
        auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
        fs = mem.get();
        REQUIRE(storage.registerFileSystem(std::move(mem)));
        disks = std::make_unique<DiskService>(storage, make_default_image_registry());
    }

    // As the config app mounts: read-only, type left to detection, a 256-byte hint.
    DiskResult mount(const std::string& path, const std::vector<std::uint8_t>& bytes)
    {
        fs->file_bytes(path) = bytes;
        MountOptions options;
        options.readOnlyRequested = true;
        options.sectorSizeHint = 256;
        return disks->mount(0, "mem", path, options);
    }

    CbmVolume volume()
    {
        return CbmVolume(
            [this](std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) {
                return disks->read_sector(0, lba, dst, dstBytes);
            },
            disks->info(0).geometry.sectorCount);
    }

    std::array<std::uint8_t, 256> sector(std::uint8_t track, std::uint8_t number)
    {
        std::uint32_t block = 0;
        REQUIRE(cbm_block_number(track, number, disk_sectors, block));
        std::array<std::uint8_t, 256> out{};
        REQUIRE(disks->read_sector(0, block, out.data(), out.size()).ok());
        return out;
    }
};

std::vector<std::uint8_t> read_file(const CbmVolume& volume, const CbmDirEntry& entry)
{
    CbmFileReader reader(volume, entry.track, entry.sector);
    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 97> chunk{};
    bool last = false;
    for (std::size_t guard = 0; guard < largest_file && !last; ++guard) {
        const std::size_t n = reader.read(chunk.data(), chunk.size(), last);
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        if (n == 0) break;
    }
    CHECK(reader.error() == CbmError::None);
    CHECK(last);
    return out;
}

} // namespace

TEST_CASE("PRG disk: mounts as a read-only 35-track disk, whatever the client's hint")
{
    Rig rig;
    // 51,173 bytes does not divide by the 256 the config app sends.
    REQUIRE(rig.mount("/empire.prg", program(51173)).ok());

    const DiskSlotInfo info = rig.disks->info(0);
    CHECK(info.type == ImageType::Prg);
    CHECK(info.readOnly);
    CHECK(info.geometry.sectorSize == 256);
    CHECK(info.geometry.sectorCount == disk_sectors);
}

TEST_CASE("PRG disk: a read-write request still mounts, read-only, and refuses writes")
{
    Rig rig;
    rig.fs->file_bytes("/rw.prg") = program(600);
    MountOptions options;
    options.readOnlyRequested = false;
    REQUIRE(rig.disks->mount(0, "mem", "/rw.prg", options).ok());
    CHECK(rig.disks->info(0).readOnly);

    const std::array<std::uint8_t, 256> sector{};
    CHECK(rig.disks->write_sector(0, 0, sector.data(), sector.size()).error == DiskError::ReadOnly);
}

TEST_CASE("PRG disk: the header and directory show the one file")
{
    Rig rig;
    const auto bytes = program(600);
    REQUIRE(rig.mount("/EMPIRE-REMAKE.PRG", bytes).ok());
    const CbmVolume volume = rig.volume();

    CbmHeader header;
    REQUIRE(volume.read_header(header) == CbmError::None);
    CHECK(header.name == "EMPIRE-REMAKE");
    CHECK(header.idField == "FN 2A");
    CHECK(header.freeBlocks == data_sectors - 3);

    std::vector<CbmDirEntry> entries;
    REQUIRE(volume.read_directory(entries) == CbmError::None);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].type == CbmFileType::Prg);
    CHECK(entries[0].closed);
    CHECK_FALSE(entries[0].locked);
    CHECK(entries[0].name == "EMPIRE-REMAKE");
    CHECK(entries[0].blocks == 3);
    CHECK(read_file(volume, entries[0]) == bytes);
}

TEST_CASE("PRG disk: a file reads back exactly, at every block boundary")
{
    const std::size_t sizes[] = {
        2,
        3,
        bytes_per_data_sector - 1,
        bytes_per_data_sector,
        bytes_per_data_sector + 1,
        2 * bytes_per_data_sector,
        2 * bytes_per_data_sector + 1,
        51173,
        65538,
        sectors_before_track_18 * bytes_per_data_sector,     // ends on the last sector before 18
        sectors_before_track_18 * bytes_per_data_sector + 1, // first data sector after it
        largest_file,
    };
    for (const std::size_t size : sizes) {
        CAPTURE(size);
        Rig rig;
        const auto bytes = program(size);
        REQUIRE(rig.mount("/f.prg", bytes).ok());
        const CbmVolume volume = rig.volume();

        std::vector<CbmDirEntry> entries;
        REQUIRE(volume.read_directory(entries) == CbmError::None);
        REQUIRE(entries.size() == 1);
        const std::size_t blocks = (size + bytes_per_data_sector - 1) / bytes_per_data_sector;
        CHECK(entries[0].blocks == blocks);
        CHECK(read_file(volume, entries[0]) == bytes);

        CbmHeader header;
        REQUIRE(volume.read_header(header) == CbmError::None);
        CHECK(header.freeBlocks == data_sectors - blocks);
    }
}

TEST_CASE("PRG disk: a file with no load address, or too big for the disk, is a bad image")
{
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, largest_file + 1}) {
        CAPTURE(size);
        Rig rig;
        CHECK(rig.mount("/bad.prg", program(size)).error == DiskError::BadImage);
        CHECK_FALSE(rig.disks->info(0).inserted);
    }
}

TEST_CASE("PRG disk: the chain steps over the directory track and ends as the format says")
{
    Rig rig;
    const std::size_t size = sectors_before_track_18 * bytes_per_data_sector + 7;
    REQUIRE(rig.mount("/f.prg", program(size)).ok());

    std::uint8_t track = 1;
    std::uint8_t number = 0;
    std::size_t blocks = 0;
    std::array<std::uint8_t, 256> sector{};
    while (true) {
        CHECK(track != 18);
        sector = rig.sector(track, number);
        ++blocks;
        REQUIRE(blocks <= data_sectors);
        if (sector[0] == 0) break;
        track = sector[0];
        number = sector[1];
    }
    CHECK(blocks == sectors_before_track_18 + 1);
    CHECK(track == 19);
    CHECK(number == 0);
    CHECK(sector[1] == 7 + 1); // the index of the last byte in use
}

TEST_CASE("PRG disk: the BAM frees exactly the sectors nothing uses")
{
    Rig rig;
    const std::size_t size = (sectors_before_track_18 + 40) * bytes_per_data_sector;
    REQUIRE(rig.mount("/f.prg", program(size)).ok());

    // The sectors in use, by walking the chain, not by the rule that made it.
    std::set<std::pair<std::uint8_t, std::uint8_t>> used{{18, 0}, {18, 1}};
    std::uint8_t track = 1;
    std::uint8_t number = 0;
    while (track != 0) {
        REQUIRE(used.insert({track, number}).second);
        const auto sector = rig.sector(track, number);
        track = sector[0];
        number = sector[1];
    }

    const auto header = rig.sector(18, 0);
    CHECK(header[0] == 18);
    CHECK(header[1] == 1);
    CHECK(header[2] == 'A');
    for (std::uint8_t t = 1; t <= 35; ++t) {
        CAPTURE(int(t));
        const std::uint8_t* entry = &header[4 + (t - 1) * 4];
        const std::uint8_t onTrack = cbm_sectors_on_track(t);
        unsigned freeHere = 0;
        for (std::uint8_t s = 0; s < 24; ++s) {
            CAPTURE(int(s));
            const bool isFree = (entry[1 + s / 8] >> (s % 8)) & 1;
            if (s >= onTrack) {
                CHECK_FALSE(isFree);
            } else {
                CHECK(isFree == (used.count({t, s}) == 0));
            }
            freeHere += isFree ? 1u : 0u;
        }
        CHECK(entry[0] == freeHere);
    }
}

TEST_CASE("PRG disk: the file's name is one a person can type")
{
    CHECK(prg_disk_name("tnfs://fujinet.diller.org/cbm/demo/EMPIRE-REMAKE.PRG") == "EMPIRE-REMAKE");
    CHECK(prg_disk_name("/games/hello.prg") == "HELLO");
    CHECK(prg_disk_name("hello.Prg") == "HELLO");
    // Bytes that mean something in a CBM file name could never be asked for.
    CHECK(prg_disk_name("/a_b c.prg") == "A-B C");
    CHECK(prg_disk_name("/what?*,:=\"$#@.prg") == "WHAT---------");
    CHECK(prg_disk_name("/a-very-long-file-name-indeed.prg") == "A-VERY-LONG-FILE");
    // Only the program extension goes: a type override can mount anything.
    CHECK(prg_disk_name("/game.bin") == "GAME.BIN");
    CHECK(prg_disk_name("/.prg") == "PROGRAM");
    CHECK(prg_disk_name("") == "PROGRAM");
}

TEST_CASE("PRG disk: a type override mounts a file of any name, named as it is called")
{
    Rig rig;
    rig.fs->file_bytes("/game.bin") = program(600);
    MountOptions options;
    options.readOnlyRequested = true;
    options.typeOverride = ImageType::Prg;
    REQUIRE(rig.disks->mount(0, "mem", "/game.bin", options).ok());
    CHECK(rig.disks->info(0).type == ImageType::Prg);

    std::vector<CbmDirEntry> entries;
    REQUIRE(rig.volume().read_directory(entries) == CbmError::None);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "GAME.BIN");
}

TEST_CASE("PRG disk: sectors past the file read as empty, and past the disk are out of range")
{
    Rig rig;
    REQUIRE(rig.mount("/f.prg", program(600)).ok());

    const auto beyondFile = rig.sector(1, 5);
    CHECK(beyondFile == std::array<std::uint8_t, 256>{});

    std::array<std::uint8_t, 256> sector{};
    CHECK(rig.disks->read_sector(0, disk_sectors, sector.data(), sector.size()).error ==
          DiskError::OutOfRange);
}

TEST_CASE("PRG disk: a source file that fails under a read reports an I/O error")
{
    Rig rig;
    REQUIRE(rig.mount("/f.prg", program(600)).ok());
    rig.fs->file_bytes("/f.prg").resize(10);

    std::array<std::uint8_t, 256> sector{};
    CHECK(rig.disks->read_sector(0, 1, sector.data(), sector.size()).error == DiskError::IoError);
    // A chain is still read from the start: the file is open, only short.
    CHECK(rig.disks->read_sector(0, 2, sector.data(), sector.size()).error == DiskError::IoError);
}
