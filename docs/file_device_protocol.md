# FileDevice Binary Protocol

This document specifies the **binary, non-JSON** request/response format used by the **FileDevice** (`WireDeviceId::FileService`, currently `0xFE`).

It is designed to be:

- **8-bit-host friendly** (no JSON, minimal parsing)
- **Transport agnostic** (works over FujiBus+SLIP today; compatible with future legacy/other transports)
- **Streaming and chunkable** (supports arbitrarily large transfers via explicit continuation)
- **Low-copy** on embedded targets (ESP32) by enabling direct read/write into packet payload buffers

This protocol sits **above** the filesystem abstraction (`IFileSystem`/`IFile`) and is **not** the same thing as filesystem implementation details.

---

## Terminology

- **Host**: the remote client sending requests (modern Python tooling, emulator, or 8-bit machine).
- **Device**: `FileDevice` in fujinet-nio.
- **FS name**: a string key in `StorageManager` (e.g. `"sd0"`, `"flash"`, `"host"`, `"tnfs0"`).
- **Path**: POSIX-style path within the selected filesystem (e.g. `"/"`, `"/FOO/BAR.TXT"`).
- **LE**: little-endian.
- **Chunking**: host requests file data in blocks using `offset` + `maxBytes`.

---

## Common Encoding Rules

### Byte order
All multi-byte numeric values are **little-endian**.

### Strings
Strings are **length-prefixed** and contain raw bytes (typically ASCII/UTF-8). They are **not null-terminated**.

- `u8 len` + `len bytes` for short strings (FS name)
- `u16 len` + `len bytes` for paths

**Note:** v1 uses `u8 nameLen` for directory entry names in `ListDirectory` to keep entries compact.
If an entry name exceeds 255 bytes, it is truncated to 255 bytes in v1.

### Versioning
Every request payload begins with:

- `u8 version`

Current version: `1`

If the device receives an unknown version, it must respond with `StatusCode::InvalidRequest`.

---

## Device and Command IDs

- Device: `WireDeviceId::FileService` (currently `0xFE`)
- Commands: `FileCommand` enum (`uint8_t`) interpreted from the low 8 bits of `IORequest.command`.

Command IDs (initial set):

| Command | ID | Purpose |
|--------:|---:|---------|
| `Stat`          | `0x01` | Query metadata for a path |
| `ListDirectory` | `0x02` | Enumerate entries in a directory |
| `ReadFile`      | `0x03` | Read bytes from a file at offset |
| `WriteFile`     | `0x04` | Write bytes to a file at offset |

(IDs are intentionally compact to allow use by 8-bit transports.)

---

## Transport Wrapping (Important)

This spec defines **only** the FileDevice **payload** format.

When carried over FujiBus/FEP-004 today:

- `IOResponse.status` is encoded by the transport as the **first FujiBus parameter** (`params[0]`), width `u8`.
- The FileDevice **response bytes defined below** are carried entirely in the FujiBus **data payload**.

So the host must check *two layers*:

1. **Transport status**: `params[0]` (maps to `StatusCode`)
2. **FileDevice payload**: begins with `version` and command-specific fields

If transport status is not `Ok`, the FileDevice payload may be empty or undefined.

(Other transports may carry status differently; the FileDevice payload remains unchanged.)

---

## Common Request Prefix

Most commands require selecting a filesystem and a path.

Common prefix:

```
u8   version            // = 1
u8   fsNameLen
u8[] fsName             // length fsNameLen
u16  pathLen            // LE
u8[] path               // length pathLen
```

Validation:
- If `fsNameLen` or `pathLen` exceeds remaining payload size → `InvalidRequest`
- `fsNameLen==0` → `InvalidRequest`
- `pathLen==0`   → `InvalidRequest`

If the filesystem name does not exist in `StorageManager`, respond `DeviceNotFound`.

---

## Common Response Prefix

Many commands return structured metadata before any variable data.

Common response prefix:

```
u8   version            // = 1
u8   flags              // command-specific; generally includes "more"/"eof" bits
u16  reserved           // = 0 for now (LE)
```

The `reserved` field is for future expansion while keeping alignment stable.

---

## Command: Stat (0x01)

Returns metadata for a file or directory.

### Request

```
[Common Request Prefix]
```

### Response

```
u8   version            // = 1
u8   flags              // bit0=isDir, bit1=exists
u16  reserved           // = 0
u64  sizeBytes          // LE, 0 for directories
u64  modifiedUnixTime   // LE seconds since epoch; 0 if unavailable
```

### Status codes

- `Ok`: metadata returned; `exists` flag indicates presence
- `InvalidRequest`: malformed payload
- `DeviceNotFound`: filesystem name not found
- `IOError`: stat failed unexpectedly

Notes:
- **Normative:** If the path does not exist, respond `Ok` with `exists=false` (preferred).
- `sizeBytes` is only meaningful for regular files in v1.

---

## Command: ListDirectory (0x02)

Enumerates entries of a directory in chunks to avoid fixed size limits.

### Request

```
[Common Request Prefix]
u16  startIndex         // LE; index of first entry to return
u16  maxEntries         // LE; max number of entries to return in this response
```

### Response

```
u8   version            // = 1
u8   flags              // bit0=more (there are more entries after this chunk)
u16  reserved           // = 0
u16  returnedCount      // LE; number of entries encoded below

repeat returnedCount times:
  u8   entryFlags       // bit0=isDir
  u8   nameLen          // basename length (0..255)
  u8[] name             // basename only (no directory prefix)
  u64  sizeBytes        // LE (0 for directories)
  u64  modifiedUnixTime // LE seconds since epoch; 0 if unavailable
```

### Status codes

- `Ok`: listing chunk returned (possibly `returnedCount=0`)
- `InvalidRequest`: malformed payload, `maxEntries=0`
- `DeviceNotFound`: filesystem name not found
- `IOError`: listDirectory failed (nonexistent directory, permission, etc.)

Notes:
- Basename-only avoids repeating the directory path per entry.
- `startIndex`/`maxEntries` make this **stateless** for the device.
- Ordering is filesystem-defined; v1 does not guarantee stable ordering across calls.

---

## Command: ReadFile (0x03)

Reads bytes from a file at an explicit offset in chunks.

### Request

```
[Common Request Prefix]
u32  offset             // LE
u16  maxBytes           // LE; requested bytes to read
```

### Response

```
u8   version            // = 1
u8   flags              // bit0=eof, bit1=truncated (read < maxBytes)
u16  reserved           // = 0
u32  offset             // LE; echoed from request
u16  dataLen            // LE; actual bytes returned
u8[] data               // length dataLen
```

### Status codes

- `Ok`: data returned (possibly `dataLen=0` with `eof=1`)
- `InvalidRequest`: malformed payload or `maxBytes=0`
- `DeviceNotFound`: filesystem name not found
- `IOError`: open/read/seek failed

Notes:
- Host continues reading by setting `offset += dataLen` until `eof=1`.
- **Normative (v1):** If `offset` is at/after EOF, respond `Ok` with `dataLen=0` and `eof=1`.
- Device should not allocate more than needed; ideally read directly into the response payload buffer.

---

## Command: WriteFile (0x04)

Writes bytes to a file at an explicit offset (supports chunked upload).

### Request

```
[Common Request Prefix]
u32  offset             // LE
u16  dataLen            // LE; number of bytes to write
u8[] data               // length dataLen
```

### Response

```
u8   version            // = 1
u8   flags              // reserved; 0 for now
u16  reserved           // = 0
u32  offset             // LE; echoed from request
u16  writtenLen         // LE; actual bytes written
```

### Status codes

- `Ok`: data written (`writtenLen` may be < requested if backend limits)
- `InvalidRequest`: malformed payload or missing bytes
- `DeviceNotFound`: filesystem name not found
- `IOError`: open/write/seek failed

Notes:
- v1 open mode convention:
  - If `offset==0`, open with truncate/create semantics.
  - If `offset>0`, open existing with read/write semantics and seek (best effort).
- Future `flags` can add explicit mode control: create, append, truncate, etc.

---

## Error Handling and Robustness

- Any malformed payload should return `StatusCode::InvalidRequest`.
- Unknown command IDs should return `StatusCode::Unsupported`.
- The device should validate that declared lengths fit within the received payload size.
- The device should not assume any payload is null-terminated.

---

## Chunking Responsibility

Chunking is **host-driven**:

- The host repeatedly calls `ReadFile` or `WriteFile` with increasing offsets.
- The device performs **no unsolicited continuation**.
- This avoids async push complexity and keeps compatibility with simple transports.

This model scales to:
- very large files
- disk image reads/writes
- remote/network filesystems
- constrained RAM platforms

---

## Relationship to Disk Emulation

This file protocol is deliberately aligned with future disk-image handling:

- disk images (ATR, D64, SSD, etc.) can be opened as files
- sector reads/writes map naturally to `ReadFile/WriteFile` with computed offsets
- a future `DiskDevice` can internally reuse the same chunking semantics

---

## Future Extensions (Non-breaking)

Reserved fields (`flags`, `reserved`) and versioning allow future expansion:

- directory cookies / handles for stable enumeration
- open/read/close handles for performance
- richer metadata (attributes, permissions, long filenames)
- filesystem mount discovery / list filesystems
- negotiated maximum payload sizes per transport
- write flush / fsync semantics
