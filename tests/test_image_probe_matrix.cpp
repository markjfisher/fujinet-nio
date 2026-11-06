#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_probers/image_probe.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/disk_codec.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Image detection policy (docs/disk_device_protocol.md, "Detection policy"),
// one row per real-world variant:
//
//  1. Content probes win, whatever the extension, and over a client's hint
//     (ATR, DiskCopy 4.2, old-map ADFS, FAT, DFS catalogues).
//  2. Only extensions that mean the same thing on every machine may imply
//     geometry (.atr, .ssd, .dsd, .adf, .hda, .hfv, .xfd). Where the format's
//     sector size varies (.hda, .hfv, .xfd), a client hint that fits the file
//     takes precedence over that inference.
//  3. Nothing is guessed. Ambiguous media (.img/.ima/.raw, .dsk, unknown
//     extensions) get geometry only from content or the client's sector size
//     hint; otherwise the mount fails with GeometryRequired, or with
//     InvalidGeometry when the client's hint does not fit the file.
//
// Each row states both the probe outcome and the exact error a client gets
// from a mount. A new probe or extension must keep every row passing, or
// change the row deliberately.

using namespace fujinet::disk;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put_le16(Bytes& v, std::size_t at, std::uint16_t x)
{
    v[at] = static_cast<std::uint8_t>(x);
    v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}

void put_be32(Bytes& v, std::size_t at, std::uint32_t x)
{
    v[at] = static_cast<std::uint8_t>(x >> 24);
    v[at + 1] = static_cast<std::uint8_t>(x >> 16);
    v[at + 2] = static_cast<std::uint8_t>(x >> 8);
    v[at + 3] = static_cast<std::uint8_t>(x);
}

Bytes zeros(std::size_t n) { return Bytes(n, 0); }

// Atari ATR: 16-byte header, then 720 single-density 128-byte sectors.
Bytes atr_sd()
{
    Bytes v(16 + 720 * 128, 0);
    put_le16(v, 0, 0x0296);
    put_le16(v, 2, static_cast<std::uint16_t>((720 * 128) / 16));
    put_le16(v, 4, 128);
    return v;
}

// Apple DiskCopy 4.2: 84-byte header, then 512-byte sectors.
Bytes dc42(std::uint32_t sectors, std::uint8_t diskFormat)
{
    Bytes v(0x54 + sectors * 512, 0);
    v[0] = 4;
    std::memcpy(&v[1], "Disk", 4);
    put_be32(v, 0x40, sectors * 512);
    v[0x50] = diskFormat;
    v[0x51] = 0x22;
    v[0x52] = 0x01;
    v[0x53] = 0x00;
    return v;
}

// PC 1.44 MB FAT12 floppy boot sector.
Bytes fat_1440k()
{
    Bytes v(2880 * 512, 0);
    v[0] = 0xEB; v[1] = 0x3C; v[2] = 0x90;
    put_le16(v, 11, 512);
    v[13] = 1;
    put_le16(v, 14, 1);
    v[16] = 2;
    put_le16(v, 17, 224);
    put_le16(v, 19, 2880);
    put_le16(v, 22, 9);
    v[510] = 0x55; v[511] = 0xAA;
    return v;
}

// BBC DFS .ssd, 80 tracks: the catalogue's sector count (800) at 0x106/0x107.
Bytes ssd_80track()
{
    Bytes v(800 * 256, 0);
    v[0x106] = static_cast<std::uint8_t>(800 >> 8);
    v[0x107] = static_cast<std::uint8_t>(800 & 0xFF);
    return v;
}

// BBC DFS .dsd: two SSD sides, track-interleaved; each side's catalogue says
// its size. Side 1's catalogue follows side 0's track 0.
Bytes dsd(std::uint32_t tracks)
{
    Bytes v(tracks * 2 * 10 * 256, 0);
    const std::uint32_t perSide = tracks * 10;
    for (std::size_t sector1 : {std::size_t{0x100}, std::size_t{10 * 256 + 0x100}}) {
        v[sector1 + 6] = static_cast<std::uint8_t>(perSide >> 8);
        v[sector1 + 7] = static_cast<std::uint8_t>(perSide & 0xFF);
    }
    return v;
}

// Acorn ADFS, old map: disc size in the free space map, root directory
// "Hugo" start and end markers.
Bytes adfs(std::uint32_t sectors)
{
    Bytes v(static_cast<std::size_t>(sectors) * 256, 0);
    v[0xFC] = static_cast<std::uint8_t>(sectors);
    v[0xFD] = static_cast<std::uint8_t>(sectors >> 8);
    v[0xFE] = static_cast<std::uint8_t>(sectors >> 16);
    std::memcpy(&v[0x201], "Hugo", 4);
    std::memcpy(&v[0x6FB], "Hugo", 4);
    return v;
}

// Amstrad CPC extended .dsk: its own header, not a flat block image.
Bytes cpc_extended(std::size_t size)
{
    Bytes v(size, 0);
    const char sig[] = "EXTENDED CPC DSK File\r\nDisk-Info\r\n";
    std::memcpy(v.data(), sig, sizeof(sig) - 1);
    return v;
}

struct Row {
    const char* variant;
    Bytes bytes;
    const char* path;
    std::uint16_t hint;
    bool matched;              // probe outcome
    ImageType type;
    std::uint16_t sectorSize;  // probe geometry (0: none)
    std::uint32_t sectorCount;
    DiskError mountError;      // what an auto-detect mount returns
};

std::vector<Row> matrix()
{
    const DiskError OK = DiskError::None;
    return {
        // Content probes, whatever the extension.
        {"ATR header", atr_sd(), "/game.atr", 0, true, ImageType::Atr, 128, 720, OK},
        {"DiskCopy 4.2 800K named .dsk", dc42(1600, 1), "/mac.dsk", 0, true, ImageType::DiskCopy42, 512, 1600, OK},
        {"DiskCopy 4.2 800K named .image", dc42(1600, 1), "/mac.image", 0, true, ImageType::DiskCopy42, 512, 1600, OK},
        {"FAT 1.44M floppy .img", fat_1440k(), "/dos.img", 0, true, ImageType::Raw, 512, 2880, OK},
        {"Acorn ADFS L .adl", adfs(2560), "/master.adl", 0, true, ImageType::Raw, 256, 2560, OK},
        {"Acorn ADFS S named .adf", adfs(640), "/acorn.adf", 0, true, ImageType::Raw, 256, 640, OK},
        {"Acorn ADFS M, client hint 512 (ignored)", adfs(1280), "/games.adm", 512, true, ImageType::Raw, 256, 1280, OK},

        // Unambiguous extensions imply geometry.
        {"Amiga DD .adf", zeros(1760 * 512), "/wb.adf", 0, true, ImageType::Raw, 512, 1760, OK},
        {"Commodore 35-track .d64", zeros(683 * 256), "/game.d64", 0, true, ImageType::D64, 256, 683, OK},
        {"Commodore 35-track .d64 with error table", zeros(683 * 257), "/game.D64", 0, true, ImageType::D64, 256, 683, OK},
        {"Commodore 40-track .d64", zeros(768 * 256), "/big.d64", 0, true, ImageType::D64, 256, 768, OK},
        {"Commodore 40-track .d64 with error table", zeros(768 * 257), "/big.d64", 0, true, ImageType::D64, 256, 768, OK},
        {"Commodore 42-track .d64", zeros(802 * 256), "/big42.d64", 0, true, ImageType::D64, 256, 802, OK},
        {"Commodore .d64, client hint 512 (ignored)", zeros(683 * 256), "/game.d64", 512, true, ImageType::D64, 256, 683, OK},
        {"BBC DFS .ssd", ssd_80track(), "/bbc.ssd", 0, true, ImageType::Ssd, 256, 800, OK},
        {"BBC DFS 80-track .dsd", dsd(80), "/bbc.dsd", 0, true, ImageType::Dsd, 0, 0, OK},
        {"BBC DFS 40-track .dsd", dsd(40), "/bbc40.dsd", 0, true, ImageType::Dsd, 0, 0, OK},
        {"Mac HD .hda", zeros(2048 * 512), "/hd20.hda", 0, true, ImageType::Raw, 512, 2048, OK},
        {"Mac HD .HFV (case-insensitive)", zeros(2048 * 512), "/vol.HFV", 0, true, ImageType::Raw, 512, 2048, OK},
        {"Atari SD .xfd", zeros(720 * 128), "/sd.xfd", 0, true, ImageType::Raw, 128, 720, OK},
        {"Atari ED .xfd", zeros(1040 * 128), "/ed.xfd", 0, true, ImageType::Raw, 128, 1040, OK},
        {"Atari DD .xfd", zeros(720 * 256), "/dd.XFD", 0, true, ImageType::Raw, 256, 720, OK},

        // A hint takes precedence over inference where the sector size varies,
        // but never over content or a fixed-size format.
        {"standard-size .xfd, client hint 256 (other layout)", zeros(720 * 128), "/sd.xfd", 256, true, ImageType::Raw, 256, 360, OK},
        {"Mac .hda, client hint 1024", zeros(2048 * 512), "/hd.hda", 1024, true, ImageType::Raw, 1024, 1024, OK},
        {"Amiga .adf, client hint 256 (ignored)", zeros(1760 * 512), "/wb.adf", 256, true, ImageType::Raw, 512, 1760, OK},
        {"FAT .img, client hint 256 (ignored)", fat_1440k(), "/dos.img", 256, true, ImageType::Raw, 512, 2880, OK},

        // Ambiguous media: geometry from the client's hint, or an exact error.
        {"headerless .img, no hint", zeros(1440 * 512), "/blank.img", 0, false, ImageType::Auto, 0, 0, DiskError::GeometryRequired},
        {"headerless .img, client hint 512", zeros(1440 * 512), "/blank.img", 512, true, ImageType::Raw, 512, 1440, OK},
        {"headerless .img, hint that does not fit", zeros(1000), "/odd.img", 512, false, ImageType::Auto, 0, 0, DiskError::InvalidGeometry},
        {"Apple II 140K .dsk, no hint", zeros(143360), "/dos33.dsk", 0, false, ImageType::Auto, 0, 0, DiskError::GeometryRequired},
        {"Apple II 140K .dsk, client hint 256", zeros(143360), "/dos33.dsk", 256, true, ImageType::Raw, 256, 560, OK},
        {"Mac volume named .dsk, no hint", zeros(2048 * 512), "/hd.dsk", 0, false, ImageType::Auto, 0, 0, DiskError::GeometryRequired},
        {"Mac volume named .dsk, client hint 512", zeros(2048 * 512), "/hd.dsk", 512, true, ImageType::Raw, 512, 2048, OK},
        {"DD .xfd with 128-byte boot sectors, no hint", zeros(3 * 128 + 717 * 256), "/dd3.xfd", 0, false, ImageType::Auto, 0, 0, DiskError::GeometryRequired},
        {"non-standard .xfd, client hint 128", zeros(3 * 128 + 717 * 256), "/dd3.xfd", 128, true, ImageType::Raw, 128, 1437, OK},
        {".xfd, hint that does not fit", zeros(720 * 128), "/sd.xfd", 1000, false, ImageType::Auto, 0, 0, DiskError::InvalidGeometry},
        {"Mac .hda, hint that does not fit", zeros(2048 * 512), "/hd.hda", 1000, false, ImageType::Auto, 0, 0, DiskError::InvalidGeometry},
        {"CPC extended .dsk", cpc_extended(194816), "/cpc.dsk", 0, false, ImageType::Auto, 0, 0, DiskError::GeometryRequired},
        {"CPC extended .dsk, 512-aligned size", cpc_extended(390 * 512), "/cpc.dsk", 0, false, ImageType::Auto, 0, 0, DiskError::GeometryRequired},

        // Near misses are rejected, not reinterpreted.
        {"Amiga .adf of a size no ADF has", zeros(1000 * 512), "/bad.adf", 0, true, ImageType::Raw, 512, 0, DiskError::BadImage},
        {"DiskCopy-like header, unknown disk format", dc42(1600, 7), "/bad.dsk", 0, false, ImageType::Auto, 0, 0, DiskError::GeometryRequired},
        {"Commodore .d64 of a size no D64 has", zeros(1000 * 256), "/bad.d64", 0, true, ImageType::D64, 0, 0, DiskError::BadImage},
        {"Mac .hda not whole blocks", zeros(1000), "/odd.hda", 0, false, ImageType::Auto, 0, 0, DiskError::GeometryRequired},
    };
}

} // namespace

TEST_CASE("Image probe matrix: each known variant gets exactly the policy outcome")
{
    for (const auto& row : matrix()) {
        const std::string variant = row.variant;
        CAPTURE(variant);
        Bytes bytes = row.bytes;
        fujinet::tests::MemoryFile file(bytes, true, nullptr, nullptr);
        MountOptions opts{};
        opts.sectorSizeHint = row.hint;
        const auto r = probe_image(file, bytes.size(), row.path, opts);
        CHECK(r.matched == row.matched);
        if (!row.matched) continue;
        CHECK(r.type == row.type);
        CHECK(r.geometry.sectorSize == row.sectorSize);
        CHECK(r.geometry.sectorCount == row.sectorCount);
    }
}

TEST_CASE("Image probe matrix: each variant mounts, or fails with its exact error")
{
    fujinet::fs::StorageManager storage;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* mem = owned.get();
    REQUIRE(storage.registerFileSystem(std::move(owned)));
    DiskService svc(storage, make_default_image_registry());

    for (const auto& row : matrix()) {
        const std::string variant = row.variant;
        CAPTURE(variant);
        mem->file_bytes(row.path) = row.bytes;
        MountOptions opts{};
        opts.sectorSizeHint = row.hint;
        CHECK(svc.mount(0, "mem", row.path, opts).error == row.mountError);
        svc.unmount(0);
    }
}

TEST_CASE("Image probe matrix: an explicit Raw mount never guesses a sector size")
{
    fujinet::fs::StorageManager storage;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    owned->file_bytes("/dos33.dsk") = zeros(143360);
    owned->file_bytes("/sd.xfd") = zeros(720 * 128);
    REQUIRE(storage.registerFileSystem(std::move(owned)));
    DiskService svc(storage, make_default_image_registry());

    MountOptions raw{};
    raw.typeOverride = ImageType::Raw;
    // No content, no unambiguous extension, no hint: any size would be a guess.
    CHECK(svc.mount(0, "mem", "/dos33.dsk", raw).error == DiskError::GeometryRequired);
    // A standard .xfd is inferred, as 128-byte sectors (not the old 256 default).
    REQUIRE(svc.mount(0, "mem", "/sd.xfd", raw).ok());
    CHECK(svc.info(0).geometry.sectorSize == 128);
    CHECK(svc.info(0).geometry.sectorCount == 720);

    raw.sectorSizeHint = 256;
    REQUIRE(svc.mount(1, "mem", "/dos33.dsk", raw).ok());
    CHECK(svc.info(1).geometry.sectorSize == 256);
    CHECK(svc.info(1).geometry.sectorCount == 560);

    raw.sectorSizeHint = 1000;
    CHECK(svc.mount(2, "mem", "/sd.xfd", raw).error == DiskError::InvalidGeometry);
}

TEST_CASE("Image probe matrix: content wins over a client's sector size hint")
{
    fujinet::fs::StorageManager storage;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    owned->file_bytes("/dos.img") = fat_1440k();
    REQUIRE(storage.registerFileSystem(std::move(owned)));
    DiskService svc(storage, make_default_image_registry());

    // A client that always sends its machine's default (CONFNIO sends 512)
    // must not override what the boot sector says.
    MountOptions opts{};
    opts.sectorSizeHint = 256;
    REQUIRE(svc.mount(0, "mem", "/dos.img", opts).ok());
    CHECK(svc.info(0).geometry.sectorSize == 512);
    CHECK(svc.info(0).geometry.sectorCount == 2880);
}

TEST_CASE("DiskDevice: a failed mount carries its DiskError in the response")
{
    namespace diskproto = fujinet::io::diskproto;
    fujinet::fs::StorageManager storage;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    owned->file_bytes("/dos33.dsk") = zeros(143360);
    REQUIRE(storage.registerFileSystem(std::move(owned)));
    fujinet::io::DiskDevice dev(storage);

    auto mount = [&](std::uint8_t slot, std::uint16_t hint, const std::string& uri) {
        std::string p;
        diskproto::write_u8(p, 1);    // version
        diskproto::write_u8(p, slot); // 1-based
        diskproto::write_u8(p, 0);    // flags
        diskproto::write_u8(p, 0);    // type: auto
        diskproto::write_u16le(p, hint);
        diskproto::write_u16le(p, static_cast<std::uint16_t>(uri.size()));
        p += uri;
        fujinet::io::IORequest req{};
        req.id = 1;
        req.deviceId = fujinet::io::protocol::to_device_id(
            fujinet::io::protocol::WireDeviceId::DiskService);
        req.command = 0x01; // Mount
        req.payload.assign(p.begin(), p.end());
        return dev.handle(req);
    };

    // The status is unchanged; the payload says exactly why.
    auto resp = mount(1, 0, "mem:/dos33.dsk");
    CHECK(resp.status == fujinet::io::StatusCode::InvalidRequest);
    CHECK(resp.payload == std::vector<std::uint8_t>{1, static_cast<std::uint8_t>(DiskError::GeometryRequired)});

    resp = mount(9, 0, "mem:/dos33.dsk");
    CHECK(resp.payload == std::vector<std::uint8_t>{1, static_cast<std::uint8_t>(DiskError::InvalidSlot)});

    resp = mount(1, 0, "mem:/missing.dsk");
    CHECK(resp.payload == std::vector<std::uint8_t>{1, static_cast<std::uint8_t>(DiskError::FileNotFound)});

    // With the client's hint it mounts, and the success payload is unchanged.
    resp = mount(1, 256, "mem:/dos33.dsk");
    CHECK(resp.status == fujinet::io::StatusCode::Ok);
    CHECK(resp.payload.size() == 12);
}
