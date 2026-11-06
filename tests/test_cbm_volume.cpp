#include "doctest.h"

#include "d64_fixture.h"
#include "fujinet/disk/cbm_volume.h"
#include "fujinet/disk/d64_image.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace fujinet::disk;
using fujinet::tests::D64Fixture;

namespace {

CbmVolume volume_on(std::vector<std::uint8_t>& bytes)
{
    const auto count = static_cast<std::uint32_t>(bytes.size() / 256);
    return CbmVolume(
        [&bytes](std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) {
            if (lba * 256ULL + dstBytes > bytes.size()) return DiskResult{DiskError::OutOfRange};
            std::copy_n(bytes.data() + lba * 256ULL, dstBytes, dst);
            return DiskResult{DiskError::None, static_cast<std::uint16_t>(dstBytes)};
        },
        count);
}

std::vector<std::uint8_t> pattern(std::size_t n)
{
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i * 7 + 1);
    return v;
}

std::vector<std::uint8_t> read_all(CbmFileReader& reader, std::size_t chunk, bool& lastSeen)
{
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> buf(chunk);
    lastSeen = false;
    for (int guard = 0; guard < 100000 && !lastSeen; ++guard) {
        const std::size_t n = reader.read(buf.data(), buf.size(), lastSeen);
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
        if (n == 0 && !lastSeen) break;
    }
    return out;
}

} // namespace

TEST_CASE("1541 layout: sectors per track and block numbers match the public format")
{
    CHECK(cbm_sectors_on_track(0) == 0);
    CHECK(cbm_sectors_on_track(1) == 21);
    CHECK(cbm_sectors_on_track(17) == 21);
    CHECK(cbm_sectors_on_track(18) == 19);
    CHECK(cbm_sectors_on_track(24) == 19);
    CHECK(cbm_sectors_on_track(25) == 18);
    CHECK(cbm_sectors_on_track(30) == 18);
    CHECK(cbm_sectors_on_track(31) == 17);
    CHECK(cbm_sectors_on_track(42) == 17);
    CHECK(cbm_sectors_on_track(43) == 0);

    std::uint32_t block = 0;
    REQUIRE(cbm_block_number(1, 0, 683, block));
    CHECK(block == 0);
    // Track 18 sector 0, the header: byte offset 0x16500 in every D64.
    REQUIRE(cbm_block_number(18, 0, 683, block));
    CHECK(block == 357);
    CHECK(block * 256 == 0x16500);
    REQUIRE(cbm_block_number(35, 16, 683, block));
    CHECK(block == 682);

    CHECK_FALSE(cbm_block_number(0, 0, 683, block));
    CHECK_FALSE(cbm_block_number(1, 21, 683, block));
    CHECK_FALSE(cbm_block_number(36, 0, 683, block));
    REQUIRE(cbm_block_number(36, 0, 768, block));
    CHECK(block == 683);
}

TEST_CASE("D64 file sizes: 35, 40 and 42 tracks, with and without an error table")
{
    DiskGeometry g{};
    REQUIRE(d64_geometry_for_size(174848, g));
    CHECK(g.sectorSize == 256);
    CHECK(g.sectorCount == 683);
    REQUIRE(d64_geometry_for_size(175531, g));
    CHECK(g.sectorCount == 683);
    REQUIRE(d64_geometry_for_size(196608, g));
    CHECK(g.sectorCount == 768);
    REQUIRE(d64_geometry_for_size(197376, g));
    CHECK(g.sectorCount == 768);
    REQUIRE(d64_geometry_for_size(205312, g));
    CHECK(g.sectorCount == 802);
    REQUIRE(d64_geometry_for_size(206114, g));
    CHECK(g.sectorCount == 802);
    CHECK_FALSE(d64_geometry_for_size(174847, g));
    CHECK_FALSE(d64_geometry_for_size(0, g));
}

TEST_CASE("CBM name matching: wildcards and lengths")
{
    CHECK(cbm_name_matches("*", "ANYTHING"));
    CHECK(cbm_name_matches("*", ""));
    CHECK(cbm_name_matches("HELLO", "HELLO"));
    CHECK_FALSE(cbm_name_matches("HELLO", "HELLO2"));
    CHECK_FALSE(cbm_name_matches("HELLO2", "HELLO"));
    CHECK(cbm_name_matches("HEL*", "HELLO"));
    CHECK(cbm_name_matches("H?LLO", "HELLO"));
    CHECK_FALSE(cbm_name_matches("H?LLO", "HLLO"));
    CHECK(cbm_name_matches("H?L*", "HELLO"));
    CHECK_FALSE(cbm_name_matches("X*", "HELLO"));
    CHECK(cbm_name_matches("", ""));
}

TEST_CASE("CbmVolume reads the header, free blocks and directory")
{
    D64Fixture disk;
    disk.name_disk("TEST DISK", "AB");
    const auto data = pattern(600);
    disk.add_prg(0, "FIRST", 0, data);
    disk.add_entry(1, 0x00, "GONE", 1, 10, 3);          // scratched: not listed
    disk.add_entry(2, 0x81, "A SEQ FILE", 1, 11, 1);
    disk.add_entry(3, 0xC2, "LOCKED", 1, 12, 2);        // closed and locked
    disk.add_entry(4, 0x02, "SPLAT", 1, 13, 4);         // never closed

    auto volume = volume_on(disk.bytes);

    CbmHeader header;
    REQUIRE(volume.read_header(header) == CbmError::None);
    CHECK(header.name == "TEST DISK");
    CHECK(header.idField == "AB 2A");
    CHECK(header.freeBlocks == 664);

    std::vector<CbmDirEntry> entries;
    REQUIRE(volume.read_directory(entries) == CbmError::None);
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].name == "FIRST");
    CHECK(entries[0].type == CbmFileType::Prg);
    CHECK(entries[0].closed);
    CHECK_FALSE(entries[0].locked);
    CHECK(entries[0].track == 1);
    CHECK(entries[0].sector == 0);
    CHECK(entries[0].blocks == 3);
    CHECK(entries[1].type == CbmFileType::Seq);
    CHECK(entries[2].locked);
    CHECK_FALSE(entries[3].closed);
}

TEST_CASE("CbmVolume starts the directory at 18/1 whatever the header sector's link says")
{
    D64Fixture disk;
    disk.add_entry(0, 0x82, "REAL", 1, 0, 1);
    auto* bam = disk.sector(18, 0);
    bam[0] = 35;
    bam[1] = 16;

    auto volume = volume_on(disk.bytes);
    std::vector<CbmDirEntry> entries;
    REQUIRE(volume.read_directory(entries) == CbmError::None);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "REAL");
}

TEST_CASE("CbmVolume follows the directory chain into a second sector")
{
    D64Fixture disk;
    disk.add_entry(0, 0x82, "ONE", 1, 0, 1);
    auto* first = disk.sector(18, 1);
    first[0] = 18;
    first[1] = 4;
    auto* second = disk.sector(18, 4);
    second[0] = 0;
    second[1] = 0xFF;
    second[2] = 0x82;
    second[3] = 1;
    second[4] = 5;
    for (std::size_t i = 0; i < 16; ++i) second[5 + i] = i < 3 ? "TWO"[i] : 0xA0;

    auto volume = volume_on(disk.bytes);
    std::vector<CbmDirEntry> entries;
    REQUIRE(volume.read_directory(entries) == CbmError::None);
    REQUIRE(entries.size() == 2);
    CHECK(entries[1].name == "TWO");
    CHECK(entries[1].sector == 5);
}

TEST_CASE("CbmVolume reports a directory chain that loops")
{
    D64Fixture disk;
    disk.add_entry(0, 0x82, "ONCE", 1, 0, 1);
    auto* first = disk.sector(18, 1);
    first[0] = 18;
    first[1] = 1;

    auto volume = volume_on(disk.bytes);
    std::vector<CbmDirEntry> entries;
    CHECK(volume.read_directory(entries) == CbmError::IllegalTrackSector);
    // Each sector counts once, and what was read before the loop is still there.
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "ONCE");
}

TEST_CASE("CbmFileReader returns the data bytes of a chain exactly, whatever the read size")
{
    for (const std::size_t length : {std::size_t{1}, std::size_t{253}, std::size_t{254},
                                     std::size_t{255}, std::size_t{508}, std::size_t{2000}}) {
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{254},
                                        std::size_t{4096}}) {
            CAPTURE(length);
            CAPTURE(chunk);
            D64Fixture disk;
            const auto data = pattern(length);
            disk.add_prg(0, "F", 0, data);
            auto volume = volume_on(disk.bytes);
            CbmFileReader reader(volume, 1, 0);
            bool last = false;
            const auto out = read_all(reader, chunk, last);
            CHECK(out == data);
            CHECK(last);
            CHECK(reader.error() == CbmError::None);
        }
    }
}

TEST_CASE("CbmFileReader flags the last read of a file, not an earlier one")
{
    D64Fixture disk;
    const auto data = pattern(300);
    disk.add_prg(0, "F", 0, data);
    auto volume = volume_on(disk.bytes);
    CbmFileReader reader(volume, 1, 0);

    std::vector<std::uint8_t> buf(299);
    bool last = true;
    CHECK(reader.read(buf.data(), buf.size(), last) == 299);
    CHECK_FALSE(last);
    std::uint8_t one = 0;
    CHECK(reader.read(&one, 1, last) == 1);
    CHECK(last);
    CHECK(one == data.back());
}

TEST_CASE("CbmFileReader stops on a chain that points off the disk or loops")
{
    SUBCASE("illegal sector")
    {
        D64Fixture disk;
        auto* s = disk.sector(1, 0);
        s[0] = 1;
        s[1] = 200;
        auto volume = volume_on(disk.bytes);
        CbmFileReader reader(volume, 1, 0);
        bool last = false;
        const auto out = read_all(reader, 64, last);
        CHECK(out.size() == 254);
        CHECK(last);
        CHECK(reader.error() == CbmError::IllegalTrackSector);
    }
    SUBCASE("loop")
    {
        D64Fixture disk;
        auto* s = disk.sector(1, 0);
        s[0] = 1;
        s[1] = 0;
        auto volume = volume_on(disk.bytes);
        CbmFileReader reader(volume, 1, 0);
        bool last = false;
        (void)read_all(reader, 4096, last);
        CHECK(last);
        CHECK(reader.error() == CbmError::IllegalTrackSector);
    }
}

namespace {

std::string hex_of(const std::vector<std::uint8_t>& bytes, std::size_t start, std::size_t count)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = start; i < start + count && i < bytes.size(); ++i) {
        if (!out.empty()) out += ' ';
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0x0F];
    }
    return out;
}

} // namespace

TEST_CASE("Directory listing matches a 1541's bytes, line for line")
{
    D64Fixture disk;
    disk.name_disk("TEST DISK", "AB");
    disk.add_entry(0, 0x82, "HELLO", 1, 0, 12);

    // Blocks free as a 1541 reads them: 652 here, as in the worked example below.
    auto* bam = disk.sector(18, 0);
    for (std::uint8_t t = 1; t <= 35; ++t) bam[4 + (t - 1) * 4] = 0;
    bam[4 + 0 * 4] = 0xFF;
    bam[4 + 1 * 4] = 0xFF;
    bam[4 + 2 * 4] = 0x8E;

    auto volume = volume_on(disk.bytes);
    CbmHeader header;
    REQUIRE(volume.read_header(header) == CbmError::None);
    REQUIRE(header.freeBlocks == 652);
    std::vector<CbmDirEntry> entries;
    REQUIRE(volume.read_directory(entries) == CbmError::None);

    const auto listing = cbm_format_directory(header, entries, "");

    // 32 bytes for each of the entries, the header and the footer, load address included.
    REQUIRE(listing.size() == 32 * 3);
    CHECK(hex_of(listing, 0, 2) == "01 08");

    // Header: link, line 0, reverse on, quoted name padded with spaces, ID field, NUL.
    CHECK(hex_of(listing, 2, 30) ==
          "01 01 00 00 12 22 54 45 53 54 20 44 49 53 4B 20 20 20 20 20 20 20 22 20 41 42 20 32 41 00");

    // The public example of a 12-block PRG named HELLO.
    CHECK(hex_of(listing, 32, 32) ==
          "01 01 0C 00 20 20 22 48 45 4C 4C 4F 22 20 20 20 20 20 20 20 20 20 20 20 20 50 52 47 20 20 20 00");

    // The public example of 652 BLOCKS FREE, ending the program.
    CHECK(hex_of(listing, 64, 32) ==
          "01 01 8C 02 42 4C 4F 43 4B 53 20 46 52 45 45 2E 20 20 20 20 20 20 20 20 20 20 20 20 20 00 00 00");
}

TEST_CASE("Directory listing marks splat and locked files and aligns the block count")
{
    D64Fixture disk;
    disk.add_entry(0, 0x01, "OPEN", 1, 0, 5);           // SEQ, never closed
    disk.add_entry(1, 0xC2, "LOCKED", 1, 1, 50);        // PRG, closed and locked
    disk.add_entry(2, 0x82, "SIXTEEN CHARS OK", 1, 2, 500);
    disk.add_entry(3, 0x84, "RELATIVE", 1, 3, 1000);
    disk.add_entry(4, 0x89, "ODD", 1, 4, 1);            // a type code no DOS uses

    auto volume = volume_on(disk.bytes);
    CbmHeader header;
    REQUIRE(volume.read_header(header) == CbmError::None);
    std::vector<CbmDirEntry> entries;
    REQUIRE(volume.read_directory(entries) == CbmError::None);

    const auto listing = cbm_format_directory(header, entries, "");
    const auto text = [&](std::size_t line) {
        const auto* start = listing.data() + 32 * line + 4;
        return std::string(reinterpret_cast<const char*>(start), 27);
    };
    CHECK(text(1) == "   \"OPEN\"            *SEQ  ");
    CHECK(text(2) == "  \"LOCKED\"           PRG<  ");
    CHECK(text(3) == " \"SIXTEEN CHARS OK\" PRG    ");
    CHECK(text(4) == " \"RELATIVE\"         REL    ");
    CHECK(text(5) == "   \"ODD\"              ???  ");
}

TEST_CASE("Directory listing shows text hidden after a shifted space past the closing quote")
{
    D64Fixture disk;
    disk.add_entry(0, 0x82, "A", 1, 0, 1);
    auto* entry = disk.sector(18, 1);
    entry[5 + 1] = 0xA0;
    entry[5 + 2] = ',';
    entry[5 + 3] = '8';

    auto volume = volume_on(disk.bytes);
    CbmHeader header;
    REQUIRE(volume.read_header(header) == CbmError::None);
    std::vector<CbmDirEntry> entries;
    REQUIRE(volume.read_directory(entries) == CbmError::None);
    CHECK(entries[0].name == "A");

    const auto listing = cbm_format_directory(header, entries, "");
    const std::string text(reinterpret_cast<const char*>(listing.data() + 32 + 4), 27);
    CHECK(text.rfind("   \"A\",8", 0) == 0);
}

TEST_CASE("Directory listing of a disk with no files still has its header and free count")
{
    D64Fixture disk;
    auto volume = volume_on(disk.bytes);
    CbmHeader header;
    REQUIRE(volume.read_header(header) == CbmError::None);
    std::vector<CbmDirEntry> entries;
    REQUIRE(volume.read_directory(entries) == CbmError::None);
    CHECK(entries.empty());

    const auto listing = cbm_format_directory(header, entries, "");
    CHECK(listing.size() == 32 * 2);
    const std::string text(listing.begin(), listing.end());
    CHECK(text.find("BLOCKS FREE.") != std::string::npos);
}

TEST_CASE("Directory listing applies a name pattern and a type filter")
{
    D64Fixture disk;
    disk.add_entry(0, 0x82, "ALPHA", 1, 0, 1);
    disk.add_entry(1, 0x81, "BETA", 1, 1, 1);
    auto volume = volume_on(disk.bytes);
    CbmHeader header;
    REQUIRE(volume.read_header(header) == CbmError::None);
    std::vector<CbmDirEntry> entries;
    REQUIRE(volume.read_directory(entries) == CbmError::None);

    {
        const auto listing = cbm_format_directory(header, entries, "B*");
        const std::string text(listing.begin(), listing.end());
        CHECK(text.find("BETA") != std::string::npos);
        CHECK(text.find("ALPHA") == std::string::npos);
    }
    {
        const auto listing = cbm_format_directory(header, entries, "*", 'P');
        const std::string text(listing.begin(), listing.end());
        CHECK(text.find("ALPHA") != std::string::npos);
        CHECK(text.find("BETA") == std::string::npos);
    }
}
