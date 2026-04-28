# Disk Subsystem + DiskDevice v1 Protocol

This document specifies the **Disk subsystem** (core, platform-agnostic) and the **DiskDevice v1** binary protocol exposed as a `VirtualDevice`.

Design goals:

- **Two worlds**:
  - **Host-visible drives**: D1:, D2:, … (sector/block interface)
  - **Where bits live**: disk image files stored on named filesystems (`StorageManager`)
- **Core-first**: image parsing + sector I/O is reusable by multiple machine-specific disk devices (Atari SIO, BBC DFS/MMFS, etc.).
- **No platform ifdefs** in shared/core code.
- **8-bit friendly protocol**: binary, fixed-endian, small parsing surface.

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
- Slots contain:
  - “inserted” state (image mounted or not)
  - readonly state (requested vs effective)
  - “dirty” flag (writes occurred since mount)
  - “changed” flag (mount/unmount toggles; host can clear it)
  - image geometry (sector size, sector count)
  - last error (`disk::DiskError`)

---

## Image types and autodetect

Disk images are handled by `IDiskImage` implementations.

v1 includes (image-format understanding required for sector I/O):

- **Raw** (`ImageType::Raw`): flat `sector_count * sector_size` bytes, **no header**
  - used for tests and tooling
  - requires `sectorSizeHint` (default 256 if not provided)

- **SSD** (`ImageType::Ssd`, `.ssd`): BBC DFS SSD image (flat 256-byte sectors)
  - supported sizes (validated on mount):
    - 40 track: 400 sectors = 102,400 bytes
    - 80 track: 800 sectors = 204,800 bytes
  - v1 scope: **geometry + sector read/write only** (no DFS catalog parsing)

Planned image formats:

- DSD (`.dsd`)

Autodetect uses file extension (case-insensitive). A host may optionally override type detection.

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
u8  flags            // bit0 = readonly_requested for this live mount request
u8  typeOverride     // 0=Auto, 1=ATR, 2=SSD, 3=DSD, 4=Raw
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
- If `bit0` is clear, the service may try writable access first and then fall back to read-only.
- The actual outcome is reported in response `flags bit1` (`readonly_effective`).

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

## Status and error mapping

`DiskDevice` returns a transport-level `StatusCode` and optionally includes a disk-level `lastError` in the `Info` payload.

General mapping:

- Invalid inputs and out-of-range values → `StatusCode::InvalidRequest`
- Missing/empty slot → `StatusCode::NotReady`
- Unsupported image type → `StatusCode::Unsupported`
- Underlying file I/O failure → `StatusCode::IOError`

---

## Command: Create (0x07)

Create a new image file on a named filesystem using a **full URI**. This command does **not** mount the created image.

### Request

```
u8  version
u8  flags            // bit0 = overwrite
u8  type             // 1=ATR, 2=SSD, 3=DSD, 4=Raw (0=Auto invalid)
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
