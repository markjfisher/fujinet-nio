#include "doctest.h"

#include "d64_fixture.h"
#include "fake_fs.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/iec_bus.h"
#include "fujinet/io/iec_drive_frontend.h"
#include "iec_test_helpers.h"

#include <memory>
#include <string>
#include <vector>

using namespace fujinet::io;
using namespace fujinet::tests;

namespace {

constexpr std::uint8_t drive = 8;
constexpr std::uint8_t command_ch = 15;

// A program of `length` bytes that starts with a load address, as PRG files do.
std::vector<std::uint8_t> program(std::size_t length)
{
    std::vector<std::uint8_t> bytes(length);
    for (std::size_t i = 0; i < length; ++i) bytes[i] = static_cast<std::uint8_t>(i * 5 + 3);
    if (length >= 2) {
        bytes[0] = 0x01;
        bytes[1] = 0x08;
    }
    return bytes;
}

// A drive 8 on a bus, over a real DiskService and a D64 in memory.
struct Rig {
    fujinet::fs::StorageManager storage;
    fujinet::tests::MemoryFileSystem* fs{nullptr};
    std::unique_ptr<fujinet::disk::DiskService> disks;
    std::unique_ptr<IecDriveFrontend> frontend;
    IecBus bus;

    Rig()
    {
        auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
        fs = mem.get();
        REQUIRE(storage.registerFileSystem(std::move(mem)));
        disks = std::make_unique<fujinet::disk::DiskService>(storage, fujinet::disk::make_default_image_registry());
        frontend = std::make_unique<IecDriveFrontend>(*disks, 0, true);
        bus.attach(drive, *frontend);
    }

    void insert(const D64Fixture& disk)
    {
        fs->file_bytes("/d.d64") = disk.bytes;
        fujinet::disk::MountOptions options;
        options.readOnlyRequested = true;
        REQUIRE(disks->mount(0, "mem", "/d.d64", options).ok());
        drain_status();
    }

    // INPUT#15: a TALK to the command channel, up to the CR.
    std::string status()
    {
        const ReadOutcome read = read_all(bus, drive, command_ch);
        atn(bus, {untalk});
        CHECK(read.eoiOnLastByte);
        return as_text(read.bytes);
    }

    void drain_status() { (void)status(); }

    // LOAD: OPEN on channel 0, read to EOI, UNTALK, CLOSE.
    ReadOutcome load(const std::string& name)
    {
        open_file(bus, drive, 0, name);
        ReadOutcome read = read_all(bus, drive, 0);
        atn(bus, {untalk});
        atn(bus, {listen(drive), close_channel(0)});
        atn(bus, {unlisten});
        atn(bus, {unlisten});
        return read;
    }

    void command(const std::string& text)
    {
        atn(bus, {listen(drive), data_channel(command_ch)});
        for (const char c : text) {
            const auto byte = static_cast<std::uint8_t>(c);
            CHECK(bus.write(&byte, 1, false) == 1);
        }
        atn(bus, {unlisten});
    }
};

D64Fixture sample_disk()
{
    D64Fixture disk;
    disk.name_disk("SAMPLE", "SD");
    disk.add_prg(0, "FIRST", 0, program(600));
    disk.add_prg(1, "SECOND", 5, program(30));
    return disk;
}

} // namespace

TEST_CASE("IEC drive: power-on status names the device, and resets to OK once read")
{
    Rig rig;
    const std::string first = rig.status();
    CHECK(first == "73,FUJINET NIO DOS,00,00\r");
    CHECK(rig.status() == "00, OK,00,00\r");
    CHECK(rig.status() == "00, OK,00,00\r");
}

TEST_CASE("IEC drive: the status position survives UNTALK, as GET# reads a byte per TALK")
{
    Rig rig;
    std::string collected;
    for (int guard = 0; guard < 64; ++guard) {
        atn(rig.bus, {talk(drive), data_channel(command_ch)});
        std::uint8_t byte = 0;
        const IecReadResult read = rig.bus.read(&byte, 1);
        atn(rig.bus, {untalk});
        REQUIRE(read.bytes == 1);
        collected.push_back(static_cast<char>(byte));
        if (read.eoi) break;
    }
    CHECK(collected == "73,FUJINET NIO DOS,00,00\r");
}

TEST_CASE("IEC drive: LOAD delivers the file exactly, with EOI on the last byte")
{
    for (const std::size_t length : {std::size_t{3}, std::size_t{254}, std::size_t{255},
                                     std::size_t{508}, std::size_t{509}, std::size_t{5000}}) {
        CAPTURE(length);
        Rig rig;
        D64Fixture disk;
        const auto data = program(length);
        disk.add_prg(0, "FILE", 0, data);
        rig.insert(disk);

        const ReadOutcome read = rig.load("FILE");
        CHECK(read.bytes == data);
        CHECK(read.eoiOnLastByte);
        CHECK(rig.status() == "00, OK,00,00\r");
    }
}

TEST_CASE("IEC drive: EOI still goes with the last byte when the final sector holds no data")
{
    Rig rig;
    D64Fixture disk;
    const auto data = program(254);
    disk.add_prg(0, "FILE", 0, data);
    // Link the only data sector to a final sector with nothing in it.
    auto* first = disk.sector(1, 0);
    first[0] = 1;
    first[1] = 1;
    auto* empty = disk.sector(1, 1);
    empty[0] = 0;
    empty[1] = 1;
    rig.insert(disk);

    const ReadOutcome read = rig.load("FILE");
    CHECK(read.bytes == data);
    CHECK(read.eoiOnLastByte);
}

TEST_CASE("IEC drive: a read larger than one byte delivers the same stream")
{
    Rig rig;
    D64Fixture disk;
    const auto data = program(1000);
    disk.add_prg(0, "FILE", 0, data);
    rig.insert(disk);

    open_file(rig.bus, drive, 0, "FILE");
    atn(rig.bus, {talk(drive), data_channel(0)});
    std::vector<std::uint8_t> got;
    for (int guard = 0; guard < 100; ++guard) {
        std::uint8_t chunk[300];
        const IecReadResult read = rig.bus.read(chunk, sizeof(chunk));
        got.insert(got.end(), chunk, chunk + read.bytes);
        if (read.eoi) break;
    }
    CHECK(got == data);
}

TEST_CASE("IEC drive: a missing file ends the first read at once, so the KERNAL says FILE NOT FOUND")
{
    Rig rig;
    rig.insert(sample_disk());

    open_file(rig.bus, drive, 0, "NOPE");
    const ReadOutcome read = read_all(rig.bus, drive, 0);
    CHECK(read.bytes.empty());
    CHECK(read.emptyEnd);
    // The KERNAL sends no UNTALK or CLOSE after that; the status channel still works.
    CHECK(rig.status() == "62, FILE NOT FOUND,00,00\r");
    CHECK(rig.status() == "00, OK,00,00\r");

    // Nor does the failed attempt get in the way of the next one.
    const ReadOutcome next = rig.load("SECOND");
    CHECK(next.bytes == program(30));
}

TEST_CASE("IEC drive: a loose PRG loads by name and by \"*\", and lists as its one file")
{
    Rig rig;
    const auto bytes = program(600);
    rig.fs->file_bytes("/hello.prg") = bytes;
    fujinet::disk::MountOptions options;
    options.readOnlyRequested = true;
    options.sectorSizeHint = 256;
    REQUIRE(rig.disks->mount(0, "mem", "/hello.prg", options).ok());
    rig.drain_status();

    const ReadOutcome byStar = rig.load("*");
    CHECK(byStar.bytes == bytes);
    CHECK(byStar.eoiOnLastByte);
    CHECK(rig.status() == "00, OK,00,00\r");

    CHECK(rig.load("HELLO").bytes == bytes);
    CHECK(rig.status() == "00, OK,00,00\r");

    const std::vector<std::uint8_t> listing = rig.load("$").bytes;
    const std::string text = as_text(listing);
    CHECK(text.find("\"HELLO\"") != std::string::npos);
    CHECK(text.find("PRG") != std::string::npos);
    const auto freeLine = text.find("BLOCKS FREE");
    REQUIRE(freeLine != std::string::npos);
    // The count is that BASIC line's number: 664 less the file's 3 blocks.
    CHECK((listing[freeLine - 2] | (listing[freeLine - 1] << 8)) == 661);

    rig.command("S0:HELLO");
    CHECK(rig.status().rfind("26,", 0) == 0);
}

TEST_CASE("IEC drive: LOAD with no disk, or a disk of another machine, is DRIVE NOT READY")
{
    SUBCASE("nothing mounted")
    {
        Rig rig;
        rig.drain_status();
        open_file(rig.bus, drive, 0, "ANY");
        CHECK(read_all(rig.bus, drive, 0).emptyEnd);
        CHECK(rig.status() == "74,DRIVE NOT READY,00,00\r");

        open_file(rig.bus, drive, 0, "$");
        CHECK(read_all(rig.bus, drive, 0).emptyEnd);
        CHECK(rig.status() == "74,DRIVE NOT READY,00,00\r");
    }
    SUBCASE("a raw image")
    {
        Rig rig;
        rig.drain_status();
        rig.fs->file_bytes("/other.img") = std::vector<std::uint8_t>(1024, 0);
        fujinet::disk::MountOptions options;
        options.sectorSizeHint = 512;
        REQUIRE(rig.disks->mount(0, "mem", "/other.img", options).ok());
        open_file(rig.bus, drive, 0, "ANY");
        CHECK(read_all(rig.bus, drive, 0).emptyEnd);
        CHECK(rig.status() == "74,DRIVE NOT READY,00,00\r");
    }
}

TEST_CASE("IEC drive: LOAD\"$\" delivers the listing, EOI on its final byte")
{
    Rig rig;
    rig.insert(sample_disk());

    const ReadOutcome read = rig.load("$");
    REQUIRE(read.bytes.size() == 32 * 4);
    CHECK(read.eoiOnLastByte);
    const std::string text = as_text(read.bytes);
    CHECK(text.find("SAMPLE") != std::string::npos);
    CHECK(text.find("SD 2A") != std::string::npos);
    CHECK(text.find("FIRST") != std::string::npos);
    CHECK(text.find("SECOND") != std::string::npos);
    CHECK(text.find("BLOCKS FREE.") != std::string::npos);
    CHECK(rig.status() == "00, OK,00,00\r");
}

TEST_CASE("IEC drive: directory name variants")
{
    Rig rig;
    rig.insert(sample_disk());

    SUBCASE("drive number alone")
    {
        CHECK(rig.load("$0").bytes.size() == 32 * 4);
    }
    SUBCASE("a pattern")
    {
        const std::string text = as_text(rig.load("$:F*").bytes);
        CHECK(text.find("FIRST") != std::string::npos);
        CHECK(text.find("SECOND") == std::string::npos);
    }
    SUBCASE("a drive and a pattern")
    {
        const std::string text = as_text(rig.load("$0:S*").bytes);
        CHECK(text.find("SECOND") != std::string::npos);
        CHECK(text.find("FIRST") == std::string::npos);
    }
    SUBCASE("a type filter")
    {
        const std::string text = as_text(rig.load("$:*=S").bytes);
        CHECK(text.find("FIRST") == std::string::npos);
        CHECK(text.find("BLOCKS FREE.") != std::string::npos);
    }
}

TEST_CASE("IEC drive: a disk with no files lists its header and free count")
{
    Rig rig;
    D64Fixture disk;
    rig.insert(disk);
    const ReadOutcome read = rig.load("$");
    CHECK(read.bytes.size() == 32 * 2);
    CHECK(read.eoiOnLastByte);
}

TEST_CASE("IEC drive: a damaged directory lists as far as it goes and the status says so")
{
    Rig rig;
    D64Fixture disk = sample_disk();
    auto* first = disk.sector(18, 1);
    first[0] = 18;
    first[1] = 1; // links to itself
    rig.insert(disk);

    const ReadOutcome read = rig.load("$");
    CHECK(read.bytes.size() == 32 * 4);
    CHECK(rig.status() == "66,ILLEGAL TRACK OR SECTOR,00,00\r");
}

TEST_CASE("IEC drive: away from channel 0, \"$\" reads track 18 as a file")
{
    Rig rig;
    D64Fixture disk = sample_disk();
    rig.insert(disk);

    open_file(rig.bus, drive, 2, "$");
    atn(rig.bus, {talk(drive), data_channel(2)});
    std::uint8_t bytes[2] = {};
    REQUIRE(rig.bus.read(&bytes[0], 1).bytes == 1);
    REQUIRE(rig.bus.read(&bytes[1], 1).bytes == 1);
    atn(rig.bus, {untalk});
    // Header sector data starts at its byte 2, the DOS version.
    CHECK(bytes[0] == 'A');
    CHECK(bytes[1] == 0);
}

TEST_CASE("IEC drive: LOAD\"*\" is the first program, whatever comes before it")
{
    Rig rig;
    D64Fixture disk;
    disk.add_entry(0, 0x81, "NOTES", 1, 3, 1);  // SEQ
    disk.add_prg(1, "APP", 0, program(40));
    rig.insert(disk);

    CHECK(rig.load("*").bytes == program(40));
    CHECK(rig.status() == "00, OK,00,00\r");
}

TEST_CASE("IEC drive: a disk with no program has no LOAD\"*\"")
{
    Rig rig;
    D64Fixture disk;
    disk.add_entry(0, 0x81, "NOTES", 1, 3, 1);
    rig.insert(disk);

    CHECK(rig.load("*").emptyEnd);
    CHECK(rig.status() == "62, FILE NOT FOUND,00,00\r");
}

TEST_CASE("IEC drive: names: drive prefix, wildcards, shifted space")
{
    Rig rig;
    rig.insert(sample_disk());

    CHECK(rig.load("0:FIRST").bytes == program(600));
    CHECK(rig.load(":SECOND").bytes == program(30));
    CHECK(rig.load("F*").bytes == program(600));
    CHECK(rig.load("0:S*").bytes == program(30));
    CHECK(rig.load("SEC?ND").bytes == program(30));
    CHECK(rig.load(std::string("FIRST\xA0IGNORED")).bytes == program(600));
    CHECK(rig.load("FIRS").emptyEnd);
    CHECK(rig.status() == "62, FILE NOT FOUND,00,00\r");
}

TEST_CASE("IEC drive: name bytes from $80 up match exactly")
{
    Rig rig;
    D64Fixture disk;
    const std::string shifted = "\xC1\xC2";
    disk.add_prg(0, shifted, 0, program(10));
    rig.insert(disk);
    CHECK(rig.load(shifted).bytes == program(10));
    CHECK(rig.load("AB").emptyEnd);
}

TEST_CASE("IEC drive: LOAD of the wrong type is FILE TYPE MISMATCH; data channels take any")
{
    Rig rig;
    D64Fixture disk;
    const auto text = program(20);
    const auto blocks = disk.write_chain(1, 0, text);
    disk.add_entry(0, 0x81, "DATA", 1, 0, blocks);
    rig.insert(disk);

    CHECK(rig.load("DATA").emptyEnd);
    CHECK(rig.status() == "64, FILE TYPE MISMATCH,00,00\r");

    open_file(rig.bus, drive, 0, "DATA,S");
    const ReadOutcome asked = read_all(rig.bus, drive, 0);
    atn(rig.bus, {untalk});
    CHECK(asked.bytes == text);

    open_file(rig.bus, drive, 2, "DATA");
    const ReadOutcome channel2 = read_all(rig.bus, drive, 2);
    atn(rig.bus, {untalk});
    CHECK(channel2.bytes == text);

    open_file(rig.bus, drive, 3, "DATA,P,R");
    CHECK(read_all(rig.bus, drive, 3).emptyEnd);
    atn(rig.bus, {untalk});
    CHECK(rig.status() == "64, FILE TYPE MISMATCH,00,00\r");
}

TEST_CASE("IEC drive: a file never closed reads only in recovery mode")
{
    Rig rig;
    D64Fixture disk;
    const auto text = program(20);
    const auto blocks = disk.write_chain(1, 0, text);
    disk.add_entry(0, 0x01, "SPLAT", 1, 0, blocks);
    rig.insert(disk);

    open_file(rig.bus, drive, 2, "SPLAT,S,R");
    CHECK(read_all(rig.bus, drive, 2).emptyEnd);
    atn(rig.bus, {untalk});
    CHECK(rig.status() == "60, WRITE FILE OPEN,00,00\r");

    open_file(rig.bus, drive, 2, "SPLAT,S,M");
    const ReadOutcome recovered = read_all(rig.bus, drive, 2);
    atn(rig.bus, {untalk});
    CHECK(recovered.bytes == text);
}

TEST_CASE("IEC drive: scratched entries and REL files do not open")
{
    Rig rig;
    D64Fixture disk;
    disk.add_entry(0, 0x00, "GONE", 1, 0, 1);
    disk.add_entry(1, 0x84, "RECORDS", 1, 2, 1);
    rig.insert(disk);

    CHECK(rig.load("GONE").emptyEnd);
    CHECK(rig.status() == "62, FILE NOT FOUND,00,00\r");

    open_file(rig.bus, drive, 2, "RECORDS");
    CHECK(read_all(rig.bus, drive, 2).emptyEnd);
    atn(rig.bus, {untalk});
    CHECK(rig.status() == "64, FILE TYPE MISMATCH,00,00\r");
}

TEST_CASE("IEC drive: a chain that goes bad ends the stream with the status set")
{
    Rig rig;
    D64Fixture disk;
    disk.add_prg(0, "BAD", 0, program(100));
    auto* first = disk.sector(1, 0);
    first[0] = 1;
    first[1] = 200;
    rig.insert(disk);

    const ReadOutcome read = rig.load("BAD");
    CHECK(read.bytes.size() == 254);
    CHECK(read.eoiOnLastByte);
    CHECK(rig.status() == "66,ILLEGAL TRACK OR SECTOR,00,00\r");
}

TEST_CASE("IEC drive: SEQ read: position and read-ahead survive UNTALK and TALK")
{
    Rig rig;
    D64Fixture disk;
    const auto text = program(30);
    const auto blocks = disk.write_chain(1, 0, text);
    disk.add_entry(0, 0x81, "DATA", 1, 0, blocks);
    rig.insert(disk);

    open_file(rig.bus, drive, 2, "DATA,S,R");
    std::vector<std::uint8_t> got;
    bool sawEoi = false;
    for (std::size_t i = 0; i < text.size() && !sawEoi; ++i) {
        atn(rig.bus, {talk(drive), data_channel(2)});
        std::uint8_t byte = 0;
        const IecReadResult read = rig.bus.read(&byte, 1);
        atn(rig.bus, {untalk});
        REQUIRE(read.bytes == 1);
        got.push_back(byte);
        sawEoi = read.eoi;
    }
    CHECK(got == text);
    CHECK(sawEoi);
}

TEST_CASE("IEC drive: channels are independent")
{
    Rig rig;
    D64Fixture disk;
    const auto a = program(400);
    const auto b = program(300);
    disk.add_entry(0, 0x81, "A", 1, 0, disk.write_chain(1, 0, a));
    disk.add_entry(1, 0x81, "B", 2, 0, disk.write_chain(2, 0, b));
    rig.insert(disk);

    open_file(rig.bus, drive, 2, "A");
    open_file(rig.bus, drive, 3, "B");

    // Interleaved reads; opening B must not disturb A's reader.
    std::vector<std::uint8_t> gotA;
    std::vector<std::uint8_t> gotB;
    for (std::size_t i = 0; i < 300; ++i) {
        std::uint8_t byte = 0;
        atn(rig.bus, {talk(drive), data_channel(2)});
        REQUIRE(rig.bus.read(&byte, 1).bytes == 1);
        atn(rig.bus, {untalk});
        gotA.push_back(byte);
        atn(rig.bus, {talk(drive), data_channel(3)});
        REQUIRE(rig.bus.read(&byte, 1).bytes == 1);
        atn(rig.bus, {untalk});
        gotB.push_back(byte);
    }
    CHECK(std::equal(gotA.begin(), gotA.end(), a.begin()));
    CHECK(gotB == b);
}

TEST_CASE("IEC drive: OPEN on an open channel replaces the file; CLOSE on a closed one is fine")
{
    Rig rig;
    rig.insert(sample_disk());

    open_file(rig.bus, drive, 2, "FIRST,P");
    open_file(rig.bus, drive, 2, "SECOND,P");
    const ReadOutcome read = read_all(rig.bus, drive, 2);
    atn(rig.bus, {untalk});
    CHECK(read.bytes == program(30));

    atn(rig.bus, {listen(drive), close_channel(9)});
    atn(rig.bus, {unlisten});
    CHECK(rig.status() == "00, OK,00,00\r");
}

TEST_CASE("IEC drive: CLOSE of the command channel closes every channel")
{
    Rig rig;
    rig.insert(sample_disk());

    open_file(rig.bus, drive, 2, "FIRST,P");
    atn(rig.bus, {listen(drive), close_channel(command_ch)});
    atn(rig.bus, {unlisten});

    CHECK(read_all(rig.bus, drive, 2).emptyEnd);
    atn(rig.bus, {untalk});
}

TEST_CASE("IEC drive: reading a channel never opened ends at once and says FILE NOT OPEN")
{
    Rig rig;
    rig.insert(sample_disk());
    rig.command("I");
    rig.drain_status();

    CHECK(read_all(rig.bus, drive, 5).emptyEnd);
    atn(rig.bus, {untalk});
    CHECK(rig.status() == "61, FILE NOT OPEN,00,00\r");
}

TEST_CASE("IEC drive: re-reading after the end of a file gets nothing and leaves the status")
{
    Rig rig;
    rig.insert(sample_disk());

    open_file(rig.bus, drive, 2, "SECOND,P");
    (void)read_all(rig.bus, drive, 2);
    atn(rig.bus, {untalk});
    rig.drain_status();

    const ReadOutcome again = read_all(rig.bus, drive, 2);
    atn(rig.bus, {untalk});
    CHECK(again.emptyEnd);
    CHECK(rig.status() == "00, OK,00,00\r");
}

TEST_CASE("IEC drive: writes are refused, and a refusal reaches the bus master")
{
    Rig rig;
    rig.insert(sample_disk());

    SUBCASE("SAVE: the name is accepted, the data is not, and the status says why")
    {
        open_file(rig.bus, drive, 1, "NEWFILE");
        CHECK(rig.status() == "26, WRITE PROTECT ON,00,00\r");

        atn(rig.bus, {listen(drive), data_channel(1)});
        const std::uint8_t byte = 0x01;
        CHECK(rig.bus.write(&byte, 1, false) == 0);
        atn(rig.bus, {unlisten});
        atn(rig.bus, {listen(drive), close_channel(1)});
        atn(rig.bus, {unlisten});
    }
    SUBCASE("a write mode on a data channel")
    {
        open_file(rig.bus, drive, 2, "NEW,S,W");
        CHECK(rig.status() == "26, WRITE PROTECT ON,00,00\r");
        open_file(rig.bus, drive, 2, "FIRST,P,A");
        CHECK(rig.status() == "26, WRITE PROTECT ON,00,00\r");
    }
    SUBCASE("with no disk it is the missing disk that is reported")
    {
        Rig empty;
        empty.drain_status();
        open_file(empty.bus, drive, 1, "NEWFILE");
        CHECK(empty.status() == "74,DRIVE NOT READY,00,00\r");
    }
    SUBCASE("channel 0 only ever reads")
    {
        CHECK(rig.load("FIRST,P,W").bytes == program(600));
    }
}

TEST_CASE("IEC drive: commands run at UNLISTEN, through OPEN or PRINT#")
{
    Rig rig;
    rig.insert(sample_disk());

    SUBCASE("OPEN 15,8,15,\"I\"")
    {
        open_file(rig.bus, drive, command_ch, "I");
        CHECK(rig.status() == "00, OK,00,00\r");
    }
    SUBCASE("PRINT#15 with its CR")
    {
        rig.command("I0\r");
        CHECK(rig.status() == "00, OK,00,00\r");
    }
    SUBCASE("nothing runs before UNLISTEN")
    {
        open_file(rig.bus, drive, 0, "NOPE"); // leaves 62 pending
        atn(rig.bus, {listen(drive), data_channel(command_ch)});
        const std::uint8_t byte = 'I';
        rig.bus.write(&byte, 1, false);
        CHECK(rig.frontend->status() == "62, FILE NOT FOUND,00,00\r");
        atn(rig.bus, {unlisten});
        CHECK(rig.frontend->status() == "00, OK,00,00\r");
    }
    SUBCASE("an empty session leaves the status alone")
    {
        open_file(rig.bus, drive, 0, "NOPE");
        atn(rig.bus, {listen(drive), data_channel(command_ch)});
        atn(rig.bus, {unlisten});
        CHECK(rig.status() == "62, FILE NOT FOUND,00,00\r");
    }
    SUBCASE("a reset command closes everything and reports power-on")
    {
        open_file(rig.bus, drive, 2, "FIRST,P");
        rig.command("UJ\r");
        CHECK(rig.status() == "73,FUJINET NIO DOS,00,00\r");
        CHECK(read_all(rig.bus, drive, 2).emptyEnd);
        atn(rig.bus, {untalk});
        rig.command("U:\r");
        CHECK(rig.status() == "73,FUJINET NIO DOS,00,00\r");
        rig.command("UI\r");
        CHECK(rig.status() == "73,FUJINET NIO DOS,00,00\r");
    }
    SUBCASE("commands that would change the disk are write protected")
    {
        for (const char* command : {"S0:FIRST\r", "N0:NAME,ID\r", "V0\r", "R0:A=B\r", "C0:A=B\r"}) {
            CAPTURE(command);
            rig.command(command);
            CHECK(rig.status() == "26, WRITE PROTECT ON,00,00\r");
        }
    }
    SUBCASE("anything else is a syntax error")
    {
        for (const char* command : {"Z\r", "M-R\r"}) {
            CAPTURE(command);
            rig.command(command);
            CHECK(rig.status() == "31,SYNTAX ERROR,00,00\r");
        }
    }
    SUBCASE("block access has no channel to use")
    {
        for (const char* command : {"B-R 2 0 18 0\r", "U1 2 0 18 0\r", "U2 2 0 18 0\r", "B-P 2 0\r"}) {
            CAPTURE(command);
            rig.command(command);
            CHECK(rig.status() == "70,NO CHANNEL,00,00\r");
        }
    }
    SUBCASE("a very long command is cut short, never refused")
    {
        rig.command(std::string(1000, 'Z'));
        CHECK(rig.status() == "31,SYNTAX ERROR,00,00\r");
    }
}

TEST_CASE("IEC drive: an empty name on a data channel, and direct access, are refused")
{
    Rig rig;
    rig.insert(sample_disk());

    atn(rig.bus, {listen(drive), open_channel(2)});
    atn(rig.bus, {unlisten});
    CHECK(rig.status() == "34,SYNTAX ERROR,00,00\r");

    open_file(rig.bus, drive, 2, "#");
    CHECK(rig.status() == "70,NO CHANNEL,00,00\r");
}

TEST_CASE("IEC drive: a reset closes channels but leaves the disk in the drive")
{
    Rig rig;
    rig.insert(sample_disk());

    open_file(rig.bus, drive, 2, "FIRST,P");
    rig.bus.reset();
    CHECK(rig.status() == "73,FUJINET NIO DOS,00,00\r");
    CHECK(read_all(rig.bus, drive, 2).emptyEnd);
    atn(rig.bus, {untalk});

    CHECK(rig.load("*").bytes == program(600));
}

TEST_CASE("IEC drive: only drive 8 answers with nothing mounted")
{
    fujinet::fs::StorageManager storage;
    auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* fs = mem.get();
    REQUIRE(storage.registerFileSystem(std::move(mem)));
    fujinet::disk::DiskService disks(storage, fujinet::disk::make_default_image_registry());

    IecDriveFrontend eight(disks, 0, true);
    IecDriveFrontend nine(disks, 1, false);
    CHECK(eight.present());
    CHECK_FALSE(nine.present());

    fs->file_bytes("/d.d64") = D64Fixture().bytes;
    REQUIRE(disks.mount(1, "mem", "/d.d64", {}).ok());
    CHECK(nine.present());
    REQUIRE(disks.unmount(1).ok());
    CHECK_FALSE(nine.present());
}

TEST_CASE("IEC drive: a pending mount makes the drive answer, and mounts on first use")
{
    fujinet::fs::StorageManager storage;
    auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* fs = mem.get();
    REQUIRE(storage.registerFileSystem(std::move(mem)));
    fujinet::disk::DiskService disks(storage, fujinet::disk::make_default_image_registry());

    fs->file_bytes("/d.d64") = sample_disk().bytes;
    IecDriveFrontend nine(disks, 1, false);
    CHECK_FALSE(nine.present());

    disks.set_pending_mount(1, "mem:/d.d64", "r", true);
    CHECK(nine.present());

    IecBus bus;
    bus.attach(9, nine);
    open_file(bus, 9, 0, "FIRST");
    const ReadOutcome read = read_all(bus, 9, 0);
    CHECK(read.bytes == program(600));
}

TEST_CASE("IEC drive: a D64 with an error table reads as stored through the service")
{
    fujinet::fs::StorageManager storage;
    auto mem = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* fs = mem.get();
    REQUIRE(storage.registerFileSystem(std::move(mem)));
    fujinet::disk::DiskService disks(storage, fujinet::disk::make_default_image_registry());

    std::vector<std::uint8_t> image = sample_disk().bytes;
    image.insert(image.end(), 683, 0x02); // an error table: never part of a sector
    fs->file_bytes("/d.d64") = image;
    REQUIRE(disks.mount(0, "mem", "/d.d64", {}).ok());
    CHECK(disks.info(0).geometry.sectorCount == 683);

    IecDriveFrontend eight(disks, 0, true);
    IecBus bus;
    bus.attach(8, eight);
    open_file(bus, 8, 0, "FIRST");
    CHECK(read_all(bus, 8, 0).bytes == program(600));

    // The last sector is the last 256 bytes of the sector area, not the table.
    std::uint8_t last[256]{};
    REQUIRE(disks.read_sector(0, 682, last, sizeof(last)).ok());
    CHECK(std::vector<std::uint8_t>(last, last + 256) ==
          std::vector<std::uint8_t>(image.begin() + 682 * 256, image.begin() + 683 * 256));
}
