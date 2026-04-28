# FujiDevice Binary Protocol

This document specifies the **binary, non-JSON** request/response format used by the **FujiDevice** (`WireDeviceId::FujiNet`, currently `0x70`).

The FujiDevice is the **configuration and control** device for FujiNet-NIO. In the current fn-rom integration it is used primarily for:

- resetting the FujiNet instance
- querying persisted FujiNet mount-slot configuration
- updating persisted FujiNet mount-slot configuration

This protocol is intentionally small and 8-bit-host friendly.

---

## Terminology

- **Host**: the remote client sending requests (for this work, fn-rom on the BBC Micro).
- **Device**: `FujiDevice` in fujinet-nio.
- **Mount slot**: a FujiNet persisted mount entry, indexed **0..7 on the wire**.
- **Persisted slot number**: internal config/YAML representation, stored as **1..8** in [`MountConfig::slot`](include/fujinet/config/fuji_config.h:29).

---

## Common Encoding Rules

### Byte order
All multi-byte numeric values are **little-endian**.

### Strings
Strings are raw bytes with an explicit length prefix and are **not null-terminated**.

For FujiDevice v1 mount commands, string lengths are `u8`:

```text
u8 len
u8[] bytes
```

### Versioning
Unlike [`FileDevice`](docs/file_device_protocol.md) and [`DiskDevice`](docs/disk_device_protocol.md), the current FujiDevice protocol does **not** carry a payload version byte.

It is a compact command-specific protocol, and versioning is currently implicit in the command definitions.

---

## Device and Command IDs

- Device: `WireDeviceId::FujiNet` (`0x70`)
- Commands: [`FujiCommand`](include/fujinet/io/devices/fuji_commands.h:6)

| Command | ID | Purpose |
|--------:|---:|---------|
| `Reset`     | `0xFF` | Request FujiNet reset/restart |
| `GetSsid`   | `0xFE` | Reserved / not currently implemented |
| `GetMounts` | `0xFD` | Enumerate persisted FujiNet mount slots |
| `SetMount`  | `0xFC` | Create, update, or remove one persisted mount slot |
| `GetMount`  | `0xFB` | Query one persisted mount slot |

---

## Transport Wrapping

As with other devices carried over FujiBus:

- `IOResponse.status` is exposed by the transport as FujiBus status metadata
- the payload bytes defined below are carried in the FujiBus data payload

The host must validate both:

1. transport-level status
2. command-specific FujiDevice payload

---

## Command: Reset (`0xFF`)

Requests a FujiNet reset.

### Request

```text
(no payload)
```

### Response

```text
(no payload)
```

### Status codes

- `Ok`: reset request accepted
- `Unsupported`: if the device implementation does not support reset handling

Notes:
- On POSIX builds this usually means a controlled process restart path.
- On MCU targets this may reboot the device and never return to the caller.

---

## Command: GetMounts (`0xFD`)

Returns the persisted FujiNet mount table.

### Request

```text
(no payload)
```

### Response

```text
u8 count
repeat count times:
  u8 slotIndex        // 0..7 on the wire
  u8 flags            // bit0=enabled
  u8 uriLen
  u8[] uri
  u8 modeLen
  u8[] mode
```

### Status codes

- `Ok`: table returned
- `InternalError`: table could not be encoded safely

Notes:
- Returned entries are sorted ascending by persisted slot number.
- Only valid persisted mount entries are returned.
- Slot indices are translated from persisted [`MountConfig::slot`](include/fujinet/config/fuji_config.h:29) values into **0-based wire indices**.

---

## Command: GetMount (`0xFB`)

Returns one persisted FujiNet mount slot.

### Request

```text
u8 slotIndex         // 0..7
```

### Response

```text
u8 slotIndex         // echoed slot index
u8 flags             // bit0=enabled
u8 uriLen
u8[] uri
u8 modeLen
u8[] mode
```

### Status codes

- `Ok`: record returned
- `InvalidRequest`: malformed payload or slot index out of range

Notes:
- If the slot has no persisted mount entry, the response is still `Ok`.
- In that case, the record is returned as:

```text
slotIndex = requested slot
flags     = 0
uriLen    = 0
modeLen   = 1
mode      = "r"
```

This makes the command easy for small ROM clients to consume.

---

## Command: SetMount (`0xFC`)

Creates, updates, or removes one persisted FujiNet mount slot.

### Request

```text
u8 slotIndex         // 0..7
u8 flags             // bit0=enabled
u8 uriLen
u8[] uri
u8 modeLen
u8[] mode
```

### Semantics

- `slotIndex` is always **0-based on the wire**.
- Internally, FujiDevice converts that to persisted config slot numbering using [`MountConfig::from_index()`](include/fujinet/config/fuji_config.h:42).
- If `modeLen == 0`, the device normalizes mode to `"r"`.
- The `mode` field is persisted slot metadata/default policy. It is distinct from the live access mode requested through DiskDevice `Mount`.
- If `uriLen == 0`, the persisted entry for that slot is removed.
- If `uriLen > 0`, the slot is upserted and the config is saved immediately.
- `enabled` is only meaningful when `uriLen > 0`.

### Response

```text
(no payload)
```

### Status codes

- `Ok`: update/removal persisted successfully
- `InvalidRequest`: malformed payload or slot index out of range

---

## Reserved Command: GetSsid (`0xFE`)

This command identifier exists in [`FujiCommand`](include/fujinet/io/devices/fuji_commands.h:8) but is not currently implemented by [`FujiDevice`](src/lib/fuji_device.cpp:33).

### Current behavior

- requests currently return `Unsupported`

---

## Config Mapping Notes

The persisted mount data is stored in [`FujiConfig::mounts`](include/fujinet/config/fuji_config.h:74) as [`MountConfig`](include/fujinet/config/fuji_config.h:28) entries.

Important mapping rules:

- wire protocol slot index: `0..7`
- persisted config slot number: `1..8`
- conversion helpers:
  - [`MountConfig::effective_slot()`](include/fujinet/config/fuji_config.h:35) maps persisted `1..8` → wire/runtime `0..7`
  - [`MountConfig::from_index()`](include/fujinet/config/fuji_config.h:42) maps wire/runtime `0..7` → persisted `1..8`

This is specifically intended to make 8-bit client code simpler while preserving config compatibility.

---

## Error Handling and Robustness

- malformed payloads should return `StatusCode::InvalidRequest`
- unknown command IDs should return `StatusCode::Unsupported`
- slot indices outside `0..7` must return `StatusCode::InvalidRequest`
- hosts should not assume any strings are null-terminated

---

## Intended fn-rom usage

The current fn-rom design uses FujiDevice for the persisted FujiNet mount table and FileDevice for path traversal:

- [`*FIN`](../../bbc/fn-rom/src/commands/cmd_fin.s) writes a URI into a persisted FujiNet mount slot via `SetMount`, currently storing default slot policy `auto`
- [`*FMOUNT`](../../bbc/fn-rom/src/commands/cmd_fmount.s) bridges a FujiNet mount slot to one of the BBC’s local DFS drives
- [`*FHOST`](../../bbc/fn-rom/src/commands/cmd_fhost.s), `*FCD`, `*FLIST`, and `*FLS` use FileDevice URI/path resolution and listing

This split keeps URI parsing and traversal logic on the more capable FujiNet side while leaving the ROM comparatively small.
