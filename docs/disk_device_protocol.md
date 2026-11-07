# Disk Subsystem + DiskDevice v1 Protocol

This document specifies the **Disk subsystem** (core, platform-agnostic) and the **DiskDevice v1** binary protocol exposed as a `VirtualDevice`.

Design goals:

- **Two worlds**:
  - **Host-visible drives**: D1:, D2:, … (sector/block interface)
  - **Where bits live**: disk image files stored on named filesystems (`StorageManager`)
- **Core-first**: image parsing + sector I/O is reusable by multiple machine-specific disk devices (Atari SIO, BBC DFS/MMFS, etc.).
- **No platform ifdefs** in shared/core code.
- **8-bit friendly protocol**: binary, fixed-endian, small parsing surface.

Client-side driver layering, including the distinction between FujiBus packet
protocol, SLIP stream framing, and physical channels, is defined in
[`docs/driver_architecture.md`](driver_architecture.md). DiskDevice is the
logical block-device protocol; it is independent of whether the client reaches
NIO over RS-232, TCP, Zorro, or another channel.

---

## Components and layering

The disk subsystem is split into:

- **Core disk code** (`namespace fujinet::disk`)
  - `IDiskImage`: per-format image handler (ATR/SSD/DSD/…)
  - `DiskService`: owns a fixed set of slots (default 8) and implements mount/unmount/read/write/info
  - `ImageRegistry`: maps `ImageType` → factory (mount/I/O) and `ImageType` → creator (blank image create)
- **VirtualDevice wrapper** (`namespace fujinet::io`)
  - `DiskDevice`: wraps `DiskService` and exposes a v1 command set over `IORequest`/`IOResponse`

The key separation:

> Machine-specific disk protocols **must not** re-implement image parsing or storage lookups.
> They should reuse `DiskService` (composition) and implement only their wire/bus protocol.

---

## Registry wiring and where it lives

Disk image support is selected via a registry instance passed into `DiskDevice`/`DiskService`:

- **Factories**: `ImageType` → `IDiskImage` implementation (mount + sector I/O)
- **Creators**: `ImageType` → create function (used by `Create (0x07)`)

Default wiring is intentionally **platform-owned** (mirrors `platform::make_default_network_registry()`):

- `platform::make_default_disk_image_registry()` in `src/platform/*/disk_registry.cpp`

This avoids platform `#ifdef`s inside shared/core code and allows each platform/build to:

- include/exclude formats,
- apply policy (e.g. storage constraints),
- or provide different implementations.

---

## Addressing disk images: full URI

Disk images are now addressed by a **full URI** that fujinet-nio parses internally:

- `tnfs://192.168.1.100:16384/disks/game.atr` - TNFS with host:port
- `sd0:/disks/game.atr` - SD card filesystem
- `host:/images/work.ssd` - Host POSIX filesystem

This keeps the disk subsystem independent of how filesystems are provided (POSIX, ESP32 LittleFS, SD card, TNFS, etc.) and **shifts URI parsing to fujinet-nio**, not the 8-bit host.

The `StorageManager::resolveUri()` function handles scheme parsing and authority preservation (e.g., preserving `host:port` for TNFS).

---

## Slot model

`DiskService` owns a fixed array of **slots**:

- Slot numbers are **1-based** in the DiskDevice protocol (D1=1 … Dn=n).
- These are active runtime units, not the sparse user catalogue maintained by
  config-nio. See [Slot catalogue and active disk mounts](slot_state.md).
- Slots contain:
  - “inserted” state (image mounted or not)
  - readonly state (requested vs effective)
  - “dirty” flag (one or more sectors were written successfully after the
    last successful flush)
  - “changed” flag (mount/unmount toggles; host can clear it)
  - image geometry (sector size, sector count)
  - last error (`disk::DiskError`)

When `boot.mode` is `config`, bootstrap policy installs `boot.config_uri` as a
pending mount on the active runtime disk unit, currently slot index `0`
(`D1`/`D:` depending on host convention). User-initiated mounts can replace it
later. On a local filesystem the image must exist or no mount is installed.
On a network filesystem (`FileSystemKind` for which `fs::is_network_kind()`
is true) the image is not probed, because bootstrap runs before the network
link is up; the lazy mount opens it on first access and retries while it is
still unreachable.

---

## Image types and detection

Disk images are handled by `IDiskImage` implementations.

v1 includes (image-format understanding required for sector I/O):

- **Raw** (`ImageType::Raw`): flat `sector_count * sector_size` bytes, **no header**
  - used for tests and tooling
  - probes known filesystem/image signatures to infer geometry when possible
  - never guesses a sector size: when neither content, an unambiguous
    extension nor the client's `sector_size_hint` gives the geometry, the
    mount fails with `GeometryRequired` (see the detection policy)
  - persisted runtime mounts can provide `sector_size_hint` for headerless raw
    images where geometry cannot be inferred from file content
  - `.adf` is used by both Amiga and Acorn: an Acorn ADFS image is recognized
    by content first (see ADFS below), and otherwise `.adf` is an Amiga disk,
    recognized case-insensitively as raw 512-byte media; its geometry
    is `file_size / 512` blocks and non-512-aligned files are rejected
  - `.hda` and `.hfv` (Macintosh HD20/SCSI hard disk volumes) are recognized
    case-insensitively as raw 512-byte media, unless the mount supplies a
    sector size hint; files that are not whole 512-byte blocks are left to
    the other probes
  - `.xfd` (Atari, headerless) is recognized case-insensitively by its
    standard sizes: 92,160 bytes as 720 x 128 (single density), 133,120 as
    1,040 x 128 (enhanced) and 184,320 as 720 x 256 (double). Other sizes,
    including double-density images with 128-byte boot sectors (183,936
    bytes, which a flat raw image cannot represent; use ATR), need the
    client's sector size hint
  - `.dsk` is deliberately not recognized by extension (see the detection
    policy below)

- **SSD** (`ImageType::Ssd`, `.ssd`): BBC DFS SSD image (flat 256-byte sectors)
  - supported sizes (validated on mount):
    - 40 track: 400 sectors = 102,400 bytes
    - 80 track: 800 sectors = 204,800 bytes
  - v1 scope: **geometry + sector read/write only** (no DFS catalog parsing)

- **D64** (`ImageType::D64`, `.d64`): Commodore 1541 image, flat 256-byte sectors
  in track and sector order
  - supported sizes: 35, 40 or 42 tracks (683, 768 or 802 sectors), each with or
    without the one-byte-per-sector error table at the end of the file, which is
    ignored; any other size is `BadImage`
  - v1 scope: **geometry + sector read/write only**. The DOS structures on the disk
    are read by the IEC drive, not here: see [IEC drive](iec/drive.md)

- **Commodore program disk** (`ImageType::Prg` = 8, wire type 8; mount only, no
  `Create`): a loose `.prg` file presented as a synthesised, read-only, 35-track
  1541 disk holding that one file
  - the file and the disk are named after the source: its last path element
    without `.prg`, in capitals, at most 16 characters. Characters that mean
    something in a CBM file name become `-`, so the file can be loaded by name
  - files of 2 to 168,656 bytes (664 data sectors of 254 bytes): anything shorter
    has no load address, anything longer does not fit, and both are `BadImage`
  - track 18 holds the BAM (18/0) and the directory (18/1). Data sectors run in
    block order from track 1 sector 0, stepping over track 18, with no interleave
  - sector bytes are computed per read from the source file; only the file handle
    is kept. The image is always read-only: a mount asking for read-write succeeds
    and reports read-only in the mount/info flags, and a write returns `ReadOnly`

- **DSD** (`ImageType::Dsd`, `.dsd`): BBC double-sided DFS image. Each side is
  an SSD surface (DFS drives 0 and 2 on a BBC), stored track-interleaved:
  track 0 side 0, track 0 side 1, track 1 side 0, ...
  - logical sectors run through side 0, then side 1: side 1 starts at
    `sectorCount / 2` (a client maps drive 2 to that offset)
  - the side 0 catalogue gives the size: 400 sectors a side (40 tracks) or
    800 (80 tracks), so `sectorCount` is 800 or 1600
  - like SSD, images may be truncated: sectors past the end of the file read
    as zeros and are created on write

- **ADFS**, old map (BBC S, M and L floppies of 160K, 320K and 640K, and
  old-map hard discs): served as **Raw** 256-byte sectors in ADFS logical
  order (an `.adl`'s sides are already interleaved that way)
  - recognized by content, whatever the extension (`.ads`, `.adm`, `.adl`,
    Acorn `.adf`, `.dat`): the root directory's "Hugo" start and end markers,
    and the free space map's disc size equal to the file size
  - the map checksums are deliberately not checked; real images often carry
    stale ones

- **DiskCopy 4.2** (`ImageType::DiskCopy42`, any extension): Apple DiskCopy 4.2
  image of a Mac floppy (400K/800K GCR, 720K/1440K MFM)
  - 84-byte big-endian header, then 512-byte sectors, then optional 12-byte
    per-sector tags; recognised by header content, not extension
  - tags are ignored and never written
  - flushing after writes recomputes the header's data checksum so tools that
    verify it (such as DiskCopy) still accept the image
  - no blank-image creator: `Create` with this type returns `Unsupported`

- **Atari executable boot disk** (`ImageType::XexBoot` = 6, wire type 6; mount
  only, no `Create`): an Atari binary-load file (`.xex`, `.com`, `.exe`, or any
  file starting `FF FF`) presented as a synthesised, read-only, bootable
  MyDOS disk that boots the program through MyPicoDOS's PicoBoot loader
  - 256-byte sectors, the first three 128 bytes (as in ATR; a read of sectors
    1-3 returns 128 bytes). Sector count: 720 when the file needs at most 707
    data sectors, otherwise the smallest 8m+7 that holds 3 boot sectors, the
    VTOC sector(s), 8 directory sectors and `ceil(size/253)` data sectors
    (so 300059 bytes is 1199 sectors). Files needing more than 65535 sectors,
    and empty files, are `BadImage`, as is a file without the `FF FF` header.
  - the layout equals `dir2atr -m -d -B picobootcode-dd.bin` for a stage
    directory holding only `autorun.xex`: boot sectors 1-3 are PicoBoot
    (`picobootcode-dd.bin`, MyPicoDOS 4.06, GPL v2, embedded in
    `src/lib/disk/xex_boot_image.cpp`; see `THIRD_PARTY.md`); VTOC at sector
    360 (type 2, or 2+n above 1023 sectors; further VTOC sectors 359, 358, ...
    each cover 2048 more sectors once sector 1967 is passed); directory at
    361-368 with one entry `AUTORUN XEX` (flag `$42`, `$46` above 1023
    sectors) starting at sector 4; data sectors 4..(360-n), then 369 onwards,
    each 253 data bytes plus a 3-byte trailer (next sector high, next low, byte
    count; file number 0, 16-bit links)
  - sector bytes are computed per read from the source file; only the file
    handle is kept. The image is always read-only: a mount asking for
    read-write succeeds and reports read-only in the mount/info flags, and a
    write returns `ReadOnly`
  - `tests/fixtures/xex-boot/` holds dir2atr-generated goldens;
    `generate.py` there regenerates them

Planned image formats:

- HDF/RDB semantics are intentionally not inferred from `.hdf` yet.

Detection is centralized in `ProbeRegistry`, not in individual mount callers.
The default probe order is:

- ATR header probe: content match for ATR magic/header.
- DiskCopy 4.2 probe: content match for the DiskCopy 4.2 header (magic, disk
  format, tag size) whose declared data fits in the file, whatever the extension.
- ADFS probe: content match for an old-map ADFS disc ("Hugo" directory
  markers and a free space map size equal to the file), whatever the extension.
- FAT BPB probe: content match for FAT superfloppy images, returning `ImageType::Raw` plus geometry.
- Atari executable probe: `.xex`/`.com`/`.exe` extension (an extension that is
  not a disk-image extension plus the `FF FF` header also matches). A
  header-only match is skipped for `.atr`, `.xfd`, `.img`, `.raw`, `.dsk` and
  other disk-image extensions, since a blank `$FF`-filled disk starts with it.
  `.com`/`.exe` are an exception to the unambiguous-extension rule below: they
  are never disk images on any machine, so claiming them cannot hide a disk.
- SSD DFS probe: `.ssd` path plus DFS catalogue sector-count validation.
- Extension/hint fallback: case-insensitive extension and `sector_size_hint` handling for ambiguous raw images.

`ImageRegistry` is only responsible for constructing the chosen `IDiskImage`
handler. Image handlers still own final mount validation and sector I/O. A host
may optionally override type detection with an explicit `ImageType`, but raw
geometry can still come from `sector_size_hint` or probe-provided geometry.

### Adding a new image format

To add a new disk image type:

- Add an `IDiskImage` implementation that owns final validation, geometry, and sector read/write mapping.
- Register the implementation in the platform/default `ImageRegistry`.
- Add an `IImageProbe` implementation when the format can be recognized from content, path, hints, or some combination.
- Register the probe in `make_default_probe_registry()` with stronger content probes before weaker extension/hint fallbacks.
- Add rows for the new format, and for anything it could be confused with, to
  `tests/test_image_probe_matrix.cpp`.

Keep filesystem parsing inside the image separate from block I/O unless the
parsing is required to establish image geometry or sector offsets.

#### Detection policy

One NIO serves many machines, and many image extensions are shared between
them, so detection must never guess:

1. **Content wins.** A format with a signature (ATR, DiskCopy 4.2, a FAT boot
   sector, a DFS catalogue) is recognized by its content, whatever the file is
   called, and its geometry overrides any sector size hint the client sent.
2. **Only unambiguous extensions imply geometry.** An extension may select a
   type or geometry only if it means the same thing on every machine:
   `.atr`, `.ssd`, `.dsd`, `.d64`, `.adf`, `.hda`, `.hfv`, `.xfd`. Where that
   format's sector size varies (`.hda`, `.hfv`, `.xfd`), the geometry is an
   inference, so a client's sector size hint takes precedence when it fits
   the file (an image of a standard size may have another layout). Where the
   sector size is fixed (`.adf` is always 512), the hint is ignored.
   `.prg` also claims a file, as `.com` and `.exe` do: other machines' `.prg`
   files are programs too, never disk images, so claiming it cannot hide a disk.
3. **Nothing is guessed.** `.img`, `.ima` and `.raw` are raw, but get
   geometry only from content or the client's sector size hint. An explicit
   `Raw` type is no different. Without either, the mount fails with
   `GeometryRequired`; with a hint that does not divide the file size, with
   `InvalidGeometry`. `.dsk`
   (Apple II, Macintosh, Amstrad CPC, MSX, TRS-80, ...) is not claimed by
   extension at all: content probes still recognize DiskCopy 4.2 `.dsk`
   files, and otherwise the client passes the image type and sector size its
   machine uses (for example 256 for a 140K Apple II disk).

The host client knows which machine it is, so machine-specific media knowledge
belongs there, carried as the mount's type and sector size hint (persisted
runtime mounts store the hint), not in DiskService.
`tests/test_image_probe_matrix.cpp` encodes this policy, one row per known
variant.

---

## What DiskDevice does *not* do (by design)

DiskDevice / DiskService provide a **block device** view (sectors in/out). They intentionally do **not** interpret
filesystem structures *inside* the disk image (e.g. BBC DFS catalog, directory entries, boot options).

Where higher-level “disk intelligence” belongs:

- **Image-format logic** (needed for correct sector offsets and geometry) belongs in `IDiskImage`
  - example: ATR header parsing and sector-to-file-offset mapping
- **Filesystem-on-image logic** (optional) should be a separate layer (e.g. “DFS inspector”, “ATR DOS2 inspector”)
  and can be exposed later via:
  - a debug/diagnostic service,
  - a separate VirtualDevice,
  - or a host-side tool that reads sectors and parses them.

We still want **basic structural validation** at mount time (header magic, file size sanity, geometry bounds),
but that’s different from parsing the on-disk filesystem.

---

## BBC DFS 0.90 “client emulator” tooling (host-side)

Because DiskDevice is intentionally block-level, FujiNet NIO includes **host-side tooling** that reads sectors via DiskDevice and interprets the on-disk filesystem structures.

For BBC Micro DFS 0.90, the tooling lives in:

- `py/fujinet_tools/bbc.py` (CLI)
- `py/fujinet_tools/bbc_dfs.py` (DFS 0.90 catalogue parser)

### Commands

- `fujinet ... bbc dfs info --slot N`
  - reads catalogue sectors (LBA 0 and 1) and prints title/cycle/files/boot/sectors
- `fujinet ... bbc dfs cat --slot N`
  - lists catalogue entries (name, load/exec/len/start, locked)
- `fujinet ... bbc dfs read --slot N D.NAME [--out file]`
  - reads file data by start sector + length (contiguous sectors)

### DFS 0.90 catalogue layout (2-sector)

The DFS catalogue is held in **two 256-byte sectors**, exposed here as:

- sector 0 → offsets `0x000..0x0FF`
- sector 1 → offsets `0x100..0x1FF`

#### Catalogue header

- `0x000..0x007`: first 8 bytes of disk title (padded with spaces/NULs)
- `0x100..0x103`: last 4 bytes of disk title (padded with spaces/NULs)
- `0x104`: disk cycle number (BCD)
- `0x105`: (number of catalogue entries) × 8 (offset to end of directory)
- `0x106` bits:
  - `b7..b6`: zero
  - `b5..b4`: !BOOT option (`*OPT 4`)
  - `b3`: 0=DFS/WDFS, 1=HDFS (if used)
  - `b2`: total sectors bit 10 (DFS); (number of sides)-1 (HDFS)
  - `b1..b0`: total sectors bits 9..8
- `0x107`: total sectors bits 7..0

#### File entries (per entry N)

Each entry is 8 bytes in sector 0 and 8 bytes in sector 1, with:

- sector 0: offset `0x008 + N*8`
  - `+0..+6`: filename (7 bytes) + attributes (implementation-defined by DFS variant)
  - `+7`: directory char + locked flag in bit 7
- sector 1: offset `0x108 + N*8`
  - `+0..+1`: load address bits 0..15
  - `+2..+3`: exec address bits 0..15
  - `+4..+5`: file length bits 0..15
  - `+6` bits:
    - `b7..b6`: exec address bits 17..16
    - `b5..b4`: file length bits 17..16
    - `b3..b2`: load address bits 17..16
    - `b1..b0`: start sector bits 9..8
  - `+7`: start sector bits 7..0

Notes:

- File data is assumed **contiguous** on disk (DFS constraint).
- These offsets/bit-packings are what `py/fujinet_tools/bbc_dfs.py` implements today.

---

## DiskDevice: wire IDs and versioning

- **Wire device ID**: `WireDeviceId::DiskService` (`0xFC`)
- **Version**: all request payloads begin with:

```
u8 version    // = 1
```

Byte order: all multi-byte values are **little-endian**.

Strings: **u16 length-prefixed**, raw bytes, **not** null-terminated:

```
u16 len + len bytes
```

---

## Command set (v1)

Commands are encoded in the low 8 bits of `IORequest.command` (device masks to 8-bit space).

| Command | ID | Purpose |
|--------:|---:|---------|
| `Mount`        | `0x01` | Mount an image file into a slot |
| `Unmount`      | `0x02` | Unmount a slot |
| `ReadSector`   | `0x03` | Read one sector by LBA |
| `WriteSector`  | `0x04` | Write one sector by LBA |
| `Info`         | `0x05` | Query slot status + geometry |
| `ClearChanged` | `0x06` | Clear the slot “changed” flag |
| `Create`       | `0x07` | Create a new image file (blank) |
| `RestoreBoot`  | `0x0A` | Mount configured `boot.config_uri` into a slot |
| `BeginHostSession` | `0x0B` | Start a new host-side session and restore the configured boot disk |
| `Reinitialize` | `0x0C` | Recreate the image mounted in a slot with new geometry and remount it |
| `ListMounts` | `0x0D` | Page through active runtime disk-unit mappings |

`Reinitialize` is a destructive image operation, not a machine-specific
filesystem formatter. It retains the active slot's filesystem URI, image type,
and runtime mapping, and delegates blank-image creation to that image type's
registered creator. For SSD this produces the same mountable blank DFS image as
`Create`; other image types retain their own creator semantics.

Request:

```
u8  version
u8  slot
u16 sector_size
u32 sector_count
```

The slot must already contain writable mounted media. The response uses the
same mounted-image summary fields as `Mount`: version, flags, slot, image type,
sector size, and sector count.

`ListMounts` reports DiskDevice runtime mappings, including restored lazy
mappings, rather than the obsolete `fujinet.yaml` mount list. Its request is:

```
u8  version
u8  flags              // bit0 = formatted text
u16 first_unit         // 0 with last_unit=0 means all active units
u16 last_unit
u16 start_index
u16 max_payload_bytes
```

The formatted response is:

```
u8  version
u8  flags              // bit0=more; bit1=formatted
u16 first_unit
u16 start_index
u16 entry_count
u16 entries_len
u8  entries[entries_len]
```

Each formatted entry is one newline-terminated
`<unit>: <RO|AUTO> <uri>` line. Empty runtime state returns a valid response
with zero entries and zero data bytes. These runtime unit labels are 0-based;
the 1-based slot rule below applies to commands whose request contains a
singular `slot` field.

### Slot numbering

Slot is always:

```
u8 slot   // 1-based (D1=1)
```

If slot is out of range, respond with `StatusCode::InvalidRequest`.

---

## Command: Mount (0x01)

Mount an image into a slot using a **full URI**. The fujinet-nio parses the URI to extract the filesystem and path.

### Request

```
u8  version
u8  slot
u8  flags            // bit0 = readonly_requested; bit1 = stage as a lazy pending mount
u8  typeOverride     // 0=Auto, 1=ATR, 2=SSD, 3=DSD, 4=Raw, 5=DiskCopy42, 6=XexBoot, 7=D64, 8=Prg
u16 sectorSizeHint   // for Raw; otherwise 0
u16 uriLen           // LE
u8[] uri             // length uriLen - e.g., "tnfs://192.168.1.101:16384/disk.atr" or "sd0:/games.atr"
```

Examples:
- `tnfs://192.168.1.101:16384/some/path/disk.atr` - TNFS with host:port and path
- `sd0:/disks/game.atr` - SD card filesystem
- `host:/images/test.ssd` - Host POSIX filesystem

### Response payload (on `StatusCode::Ok`)

```
u8  version
u8  flags            // bit0=mounted, bit1=readonly_effective
u16 reserved         // = 0
u8  slot
u8  typeResolved
u16 sectorSize       // LE
u32 sectorCount      // LE
```

### Status codes

- `Ok`
- `InvalidRequest` (bad slot, filesystem missing, file missing, malformed payload, etc.)
- `Unsupported` (unknown/unsupported image type)
- `IOError` (open/stat/read failures)

Mount policy notes:

- DiskDevice `Mount` carries the **live** access request.
- With `flags bit1` set, the URI is resolved and recorded but the image is not
  opened until the runtime unit is first accessed. Installing that pending
  mount ejects any image previously active in the same unit, so subsequent
  sector reads cannot continue using the replaced media.
- If `bit0` is clear, the service may try writable access first and then fall back to read-only.
- The actual outcome is reported in response `flags bit1` (`readonly_effective`).

## Obsolete YAML mount lists

The former `mounts:` list in `fujinet.yaml` is not applied at startup. User
catalogue entries belong to config-nio and are accessed through
SlotCatalogService. The service currently uses sparse AppStore records as its
private persistence mechanism. Only a URI selected for an active host drive is
sent to DiskDevice `Mount`.
There is no migration from the early-development YAML format.

## Runtime mount recovery

Explicit runtime mount changes made through the DiskDevice protocol are stored
outside `fujinet.yaml` in a small recovery file on the default persistent
filesystem. This is intentionally runtime state, not user configuration.

Runtime recovery covers:

- `Mount (0x01)`: records the mounted URI, mode, and sector-size hint.
- `Unmount (0x02)`: removes the slot from runtime recovery.
- `RestoreBoot (0x0A)`: records the restored boot/config image as the current
  runtime mount for that slot.

On FujiNet startup, `DiskDevice` restores those runtime mounts as pending lazy
mounts before applying the boot/config disk.
This keeps the device-side state aligned when FujiNet is reset while the host
computer is still running and still believes a mounted disk is present.

## Amiga block-device client profile

The Amiga client uses the existing generic `DiskDevice` at wire device ID
`0xFC`; no Amiga-specific wire ID or command is required. The driver mounts an
ADF in slot 1, reads the returned geometry (or follows with `Info`), and then
uses `ReadSector (0x03)` and `WriteSector (0x04)` with 512-byte blocks. It may
use `ClearChanged (0x06)`, `Unmount (0x02)`, and the normal runtime-mount
recovery commands as needed.

For a standard 880 KiB ADF, the geometry is `sectorSize = 512` and
`sectorCount = 1760`. A successful read returns one complete 512-byte block
unless the requested host buffer is smaller, in which case the existing
truncation flag and length apply. Writes require a complete 512-byte block.
Out-of-range, read-only, malformed, and short-buffer requests use the existing
DiskDevice status mappings. `ReadSectors`/`WriteSectors` remain available for
future throughput improvements but are not required by the first Amiga driver.

NIO exposes only the block device. AmigaDOS, MountList handling, directories,
files, OFS, and FFS remain in the Amiga driver/OS and are not parsed here.

Host drivers should send `BeginHostSession (0x0B)` during driver or OS startup.
That command declares that the host-side disk state is new, clears runtime
recovery, unmounts existing slots, and restores the configured boot/config disk
for that host session.

---

## Command: Unmount (0x02)

### Request

```
u8 version
u8 slot
```

### Response payload (on `Ok`)

```
u8  version
u8  flags=0
u16 reserved=0
u8  slot
```

---

## Command: ReadSector (0x03)

Read one sector by LBA.

### Request

```
u8  version
u8  slot
u32 lba            // LE
u16 maxBytes       // LE (host buffer limit)
```

### Response payload (on `Ok`)

```
u8  version
u8  flags          // bit0=truncated (dataLen < sectorSize due to maxBytes)
u16 reserved       // = 0
u8  slot
u32 lbaEcho        // LE
u16 dataLen        // LE
u8[] data          // length dataLen
```

Notes:
- `dataLen` may be **less than** the maximum sector size for formats with variable sector sizes (e.g. ATR with base sector size 256 has 128-byte sectors for the first three sectors). Hosts should trust `dataLen`.

### Status codes

- `Ok`
- `NotReady` (no image mounted in slot)
- `InvalidRequest` (bad slot/LBA)
- `IOError`

---

## Command: WriteSector (0x04)

Write one sector by LBA.

### Request

```
u8  version
u8  slot
u32 lba            // LE
u16 dataLen        // LE
u8[] data          // length dataLen; must include at least one full sector
```

### Response payload (on `Ok`)

```
u8  version
u8  flags=0
u16 reserved=0
u8  slot
u32 lbaEcho        // LE
u16 writtenLen     // LE (sectorSize)
```

Notes:
- `writtenLen` is the number of bytes written for that sector. For variable-sector formats, this may be 128 or 256 depending on the sector.

### Status codes

- `Ok`
- `NotReady` (no image mounted in slot)
- `InvalidRequest` (readonly slot, bad slot/LBA, data too short)
- `IOError`

---

## Command: Info (0x05)

Query slot state and geometry.

If the requested slot has a pending lazy mount, `Info` activates that mount
before reporting status. This lets clients build their local disk geometry from
the real image before issuing filesystem reads.

### Request

```
u8 version
u8 slot
```

### Response payload (on `Ok`)

```
u8  version
u8  flags          // bit0=inserted, bit1=readonly, bit2=dirty, bit3=changed
                  // bit4=hasGeometry, bit5=hasLastError
u16 reserved       // = 0
u8  slot
u8  type
u16 sectorSize     // LE (0 if unknown)
u32 sectorCount    // LE (0 if unknown)
u8  lastError      // disk::DiskError
```

---

## Command: RestoreBoot (0x0A)

Mount the configured boot/config image into the requested runtime disk slot.
This command uses `boot.config_uri` and the configured boot read-only policy; it
does not accept a URI from the caller. Host tools use this to restore the
platform config disk without hardcoding platform paths.

The command mounts immediately and marks the slot changed. Host drivers may
still need a local media-change/cache invalidation step so the client operating
system rebuilds its local BPB or equivalent disk metadata.

### Request

```
u8 version
u8 slot
```

### Response payload (on `Ok`)

Same shape as `Mount`:

```
u8  version
u8  flags          // bit0=mounted, bit1=readonly_effective
u16 reserved       // = 0
u8  slot
u8  type
u16 sectorSize
u32 sectorCount
```

Returns `NotReady` when no boot/config image has been configured for the
running `DiskDevice`.

---

## Command: BeginHostSession (0x0B)

Begin a new host-side disk session. Host drivers call this during driver or OS
initialization to make the FujiNet disk state match a freshly booted host.

The command:

- clears runtime mount recovery state,
- unmounts all runtime disk slots,
- restores the configured boot/config image into the requested slot when one is
  configured.

This is different from a FujiNet reset. A FujiNet reset should preserve and
recover runtime mounts because the host computer may still be running with
cached filesystem state. `BeginHostSession` is the explicit boundary where the
host says that cached state should be discarded.

### Request

```
u8 version
u8 slot
```

### Response payload (on `Ok`)

When a boot/config image is configured, the response has the same shape as
`Mount`:

```
u8  version
u8  flags          // bit0=mounted, bit1=readonly_effective
u16 reserved       // = 0
u8  slot
u8  type
u16 sectorSize
u32 sectorCount
```

When no boot/config image is configured, the command still succeeds after
clearing runtime state and returns:

```
u8  version
u8  flags          // = 0
u16 reserved       // = 0
u8  slot
```

---

## Status and error mapping

Every response carries a transport-level `StatusCode`. Every failed response
to a known DiskDevice command also carries the exact `disk::DiskError` as its
payload, so a client can report why without a follow-up request:

```
u8  version       // 1
u8  diskError     // disk::DiskError, below
```

Successful responses are unchanged. `Info` still reports the slot's most
recent error as `lastError`. An unknown command returns `Unsupported` with no
payload.

| Value | `DiskError` | `StatusCode` | Meaning | Client action |
|------:|-------------|--------------|---------|---------------|
| 0 | `None` | `Ok` | Success | |
| 1 | `InvalidSlot` | `InvalidRequest` | Slot number out of range | Use a slot from 1 to the unit count |
| 2 | `InvalidRequest` | `InvalidRequest` | Malformed request fields or lengths | Fix the request |
| 3 | `NoSuchFileSystem` | `InvalidRequest` | The URI names no registered filesystem | Check the URI scheme or host |
| 4 | `FileNotFound` | `InvalidRequest` | No image at that path | Check the path |
| 5 | `AlreadyExists` | `InvalidRequest` | `Create` target exists without overwrite | Set overwrite, or choose another name |
| 6 | `OpenFailed` | `IOError` | The image could not be opened | Check permissions or the network |
| 7 | `UnsupportedImageType` | `Unsupported` | A recognized type NIO cannot handle (`Create` without a creator, such as DiskCopy 4.2) | Use another format |
| 8 | `BadImage` | `InvalidRequest` | A recognized format whose content or size is invalid | The image is damaged or mislabelled |
| 9 | `InvalidGeometry` | `InvalidRequest` | The client's sector size does not fit the image | Pass the sector size this medium uses |
| 10 | `NotMounted` | `NotReady` | No image in the slot (or no boot disk configured) | Mount first |
| 11 | `ReadOnly` | `InvalidRequest` | Write to a read-only mount | Remount read-write |
| 12 | `OutOfRange` | `InvalidRequest` | Sector beyond the end of the image | Stay within the geometry |
| 13 | `IoError` | `IOError` | Reading or writing the image failed | Retry, or check the backing store |
| 14 | `InternalError` | `InternalError` | An unexpected internal state | Report it |
| 15 | `GeometryRequired` | `InvalidRequest` | NIO cannot determine the image's type or sector size from content or an unambiguous extension | Mount again with the type and sector size this machine uses |

Values are part of the wire protocol: new ones are appended, never
renumbered. fujinet-nio-lib names them `FN_DISK_ERR_*`.

---

## Command: Flush (0x0E)

Flush is additive to protocol v1; the protocol version remains 1. A peer that
predates this command returns `Unsupported`.

Request:

```
u8 version
u8 slot
```

Successful response:

```
u8  version
u8  flags=0
u16 reserved=0
u8  slot
```

Dirty means at least one sector write succeeded since the last successful
flush. A successful flush clears dirty and `lastError`. A failed flush keeps
dirty set and records `IoError`. Flushing a clean mounted image succeeds
without calling the underlying file. A partial multi-sector write becomes
dirty as soon as its first sector succeeds.

Unmount and replacement mount flush old media before removal. Flush failure
leaves that media mounted and dirty. After old media has been successfully
removed, a replacement-mount failure leaves the slot empty.

---

## Command: Create (0x07)

Create a new image file on a named filesystem using a **full URI**. This command does **not** mount the created image.

### Request

```
u8  version
u8  flags            // bit0 = overwrite
u8  type             // 1=ATR, 2=SSD, 3=DSD, 4=Raw (0=Auto invalid; 5=DiskCopy42, 6=XexBoot, 7=D64 and 8=Prg have no creator)
u16 sectorSize       // LE
u32 sectorCount      // LE
u16 uriLen           // LE
u8[] uri             // length uriLen - e.g., "sd0:/newdisk.atr"
```

### Response payload (on `Ok`)

```
u8  version
u8  flags=0
u16 reserved=0
u8  type
u16 sectorSize
u32 sectorCount
```

### Create rules (v1)

- **Raw**:
  - file size = \(sectorSize * sectorCount\)
- **SSD**:
  - `sectorSize` must be 256
  - `sectorCount` must be 400 or 800
  - created file is blank (all zeros / sparse)
- **DSD**:
  - `sectorSize` must be 256
  - `sectorCount` must be 800 or 1600 (40 or 80 tracks a side)
  - a blank DFS catalogue is written for each side
- **ATR**:
  - `sectorSize` must be 128, 256, or 512
  - a standard 16-byte ATR header is written
  - when `sectorSize == 256`, the created layout uses the classic ATR convention where the first three sectors are 128 bytes


---

## Files and entry points

- Core disk interfaces: `include/fujinet/disk/*`
- DiskService implementation: `src/lib/disk/*`
- DiskDevice: `include/fujinet/io/devices/disk_device.h`, `src/lib/disk_device.cpp`
- Registration: `src/lib/disk_device_init.cpp` via `core::register_disk_device()`

---

## Example session via python tools

### SSD DFS Catalogue

```shell
❯ scripts/fujinet -p /dev/pts/7 write --chunk 2048 --mkdirs host /images/ ../../bbc/fn-rom/test.ssd

❯ scripts/fujinet -p /dev/pts/7 disk mount --slot 1 --fs host --path images/test.ssd --ro --type auto
mounted=1 readonly=1 slot=1 type=ssd sector_size=256 sector_count=800

❯ scripts/fujinet -p /dev/pts/7 bbc dfs info --slot 1
title=BASIC cycle=0 files=22 boot=exec sectors=800

❯ scripts/fujinet -p /dev/pts/7 bbc dfs cat --slot 1
Disk: BASIC  Files: 22  Sectors: 800
$.1CREAT    load=01900 exec=08023 len=00038 start=0002
$.2WRITE    load=01900 exec=08023 len=00157 start=0003
$.3TESTWR   load=01900 exec=08023 len=002B5 start=0005
$.4MULTI    load=01900 exec=08023 len=0050E start=0008
$.5RAND     load=01900 exec=08023 len=00D01 start=000E
$.6DELETE   load=01900 exec=08023 len=00850 start=001C
$.7REUSE    load=01900 exec=08023 len=0035D start=0025
$.8OSFILE   load=01900 exec=08023 len=0131B start=0029
$.BASM      load=01900 exec=08023 len=001BA start=003D
$.BASTST    load=01900 exec=08023 len=001D1 start=003F
$.BPUTEST   load=01900 exec=08023 len=00700 start=0041
$.DELETE    load=01900 exec=08023 len=002AE start=0048
$.FBGET     load=01900 exec=08023 len=006B2 start=004B
$.FHOST     load=01900 exec=08023 len=011C4 start=0052
$.FRESET    load=01900 exec=08023 len=00AAB start=0064
$.FUJIECH   load=01900 exec=08023 len=005E6 start=006F
$.FUJITST   load=01900 exec=08023 len=0042A start=0075
$.HELLO     load=00000 exec=00000 len=00002 start=007A
$.README    load=00000 exec=00000 len=00F81 start=007B
$.SIMTEST   load=01900 exec=08023 len=00243 start=008B
$.XFILL1    load=01900 exec=08023 len=004FE start=008E
$.XFILL2    load=01900 exec=08023 len=004FE start=0093

❯ scripts/fujinet -p /dev/pts/7 bbc dfs read --slot 1 README
# FujiNet BBC BASIC Test Programs

These programs test serial communication with FujiNet devices via the b2 emulator.

## Programs

### FUJITST.bas
Simple test that:
- Configures the ACIA and SERPROC for 19200 baud
...
```
