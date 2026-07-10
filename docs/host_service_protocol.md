# HostService Binary Protocol

This document specifies the binary request/response format used by
`HostService` (`WireDeviceId::HostService`, currently `0xF0`).

`HostService` is a FujiNet management service, not a virtual hardware device.
It is addressable through the same `IORequest`/`IOResponse` routing path as
devices because the wire protocol uses `DeviceID` for all endpoints. In code it
inherits via the `VirtualService` alias:

```cpp
using VirtualService = VirtualDevice;
```

Use `VirtualService` when an endpoint manages FujiNet internal state rather
than acting as a virtual peripheral. This is a documentation and naming
distinction, not a separate dispatch mechanism.

## Purpose

`HostService` owns the current host URI, the user-display path for that host,
and a bounded LRU history of host URIs.

It exists so target-host clients such as BBC ROM commands, Atari programs, and
MS-DOS tools do not need to store or parse FujiNet host state locally. The host
clients send compact binary commands; FujiNet-NIO resolves paths, validates
directories, stores state, and updates history.

Internally, `HostState` persists this data through AppStore keys in the
`fujinet-nio` namespace. Those keys are an implementation detail. Clients must
not manipulate host state by writing reserved AppStore keys.

## Device and Command IDs

- Endpoint: `WireDeviceId::HostService` (`0xF0`)
- Commands: `HostCommand` enum (`uint8_t`) interpreted from the low 8 bits of
  `IORequest.command`

| Command | ID | Purpose |
|--------:|---:|---------|
| `GetCurrent` | `0x01` | Return the current host URI and display path |
| `SetCurrent` | `0x02` | Resolve and set the current host URI |
| `ListHistory` | `0x03` | Return printable indexed LRU history text |
| `SelectHistory` | `0x04` | Select an LRU entry by index and promote it |
| `DeleteHistory` | `0x05` | Delete an LRU entry by index |

All request and response payloads begin with:

```
u8 version // = 1
```

Unknown versions return `StatusCode::InvalidRequest`.

## GetCurrent (0x01)

Request:

```
u8 version
```

Response:

```
u8   version
u16  hostLen       // LE
u16  pathLen       // LE
u8[] host          // current canonical host URI
u8[] displayPath   // user-displayable path
```

Status codes:

- `Ok`: current host exists
- `DeviceNotFound`: no current host is stored
- `InvalidRequest`: malformed payload

## SetCurrent (0x02)

Request:

```
u8   version
u16  specLen       // LE
u8[] spec          // URI or path-like host spec
```

Response:

```
u8 version
```

Behavior:

- FujiNet-NIO resolves `spec` using `HostState`.
- The resolved target must map to a registered filesystem and be a directory.
- On success, current host and display path are stored.
- The resolved host URI is moved to the top of the LRU history, or inserted
  there if not already present.
- History is capped at 32 entries.

Status codes:

- `Ok`: host set
- `InvalidRequest`: malformed payload
- `IOError`: resolution or directory validation failed

## ListHistory (0x03)

Request:

```
u8  version
u16 offset     // LE byte offset into printable history text
u16 maxBytes   // LE, must be non-zero
```

Response:

```
u8   version
u8   flags       // bit0=more
u16  offset      // LE, echoed from request
u16  dataLen     // LE
u8[] text        // printable indexed history
```

The text format is intentionally simple for small clients:

```
0 tnfs://server/current
1 tnfs://server/previous
```

Clients may print this directly. They should treat the text format as display
text, not as the storage format.

Status codes:

- `Ok`: response contains zero or more bytes of history text
- `InvalidRequest`: malformed payload or `maxBytes==0`
- `IOError`: history could not be read

## SelectHistory (0x04)

Request:

```
u8 version
u8 index       // 0..31
```

Response:

```
u8 version
```

Behavior:

- Selects the indexed history entry as current host.
- Uses the same validation path as `SetCurrent`.
- Moves the selected URI to the top of the LRU history.

Status codes:

- `Ok`: host selected
- `InvalidRequest`: malformed payload
- `IOError`: index missing, out of range, or selected host could not be set

## DeleteHistory (0x05)

Request:

```
u8 version
u8 index       // 0..31
```

Response:

```
u8 version
```

Behavior:

- Deletes the indexed LRU entry.
- Remaining entries close the gap; there are no empty slots.
- Deleting the top history entry does not change `current-host`.

Status codes:

- `Ok`: entry deleted
- `InvalidRequest`: malformed payload
- `IOError`: index missing or out of range

## Layering Notes

- `HostService` is the client-facing management API.
- `HostState` owns resolution, validation, persistence, and LRU mutation.
- `AppStore` remains plain namespaced key/value storage.
- File, disk, and other devices may use `HostState` to resolve empty or
  relative path specs, but clients should not rely on AppStore key names to
  control host behavior.

This separation keeps host-state management out of `FujiDevice` and avoids
growing a single god-class endpoint for unrelated FujiNet internals.
