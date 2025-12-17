# NetworkDevice Binary Protocol

This document specifies the **binary, non-JSON** request/response format used by the **NetworkDevice** (`WireDeviceId::NetworkService`, proposed `0xFD`).

Design goals:

- **8-bit-host friendly** (no JSON, minimal parsing)
- **Transport agnostic** (FujiBus+SLIP today; compatible with future transports)
- **Streaming and chunkable** (arbitrary response sizes via explicit offsets)
- **Poll-driven** (device progresses network work in `poll()`; host pulls results)
- **Explicit state** (network operations are represented by handles)

This protocol sits **above** any HTTP/TLS implementation and **above** any socket stack. It is purely a device protocol.

---

## Terminology

- **Host**: remote client sending requests (Python tooling, emulator, 8-bit machine).
- **Device**: `NetworkDevice` in fujinet-nio.
- **Handle**: `u16` session identifier allocated by `Open` and released by `Close`.
- **LE**: little-endian.
- **Chunking**: host requests data in blocks using `offset` + `maxBytes`.

---

## Common Encoding Rules

### Byte order
All multi-byte numeric values are **little-endian**.

### Strings
Strings are **length-prefixed** and contain raw bytes (typically ASCII/UTF-8). They are **not null-terminated**.

- `u16 len` + `len bytes` for URLs, header keys/values, etc.

### Versioning
Every request payload begins with:

- `u8 version`

Current version: `1`

If the device receives an unknown version, it must respond with `StatusCode::InvalidRequest`.

---

## Status and Transport Convention

This protocol does **not** define how status is transported. In fujinet-nio today:

- `StatusCode` is carried by the **transport** as FujiBus response `param[0]`
- the NetworkDevice protocol data is carried in the **FujiBus packet payload**

This keeps the NetworkDevice protocol independent of FujiBus/FEP-004 details.

---

## Device and Command IDs

- Device: `WireDeviceId::NetworkService` (proposed `0xFD`)
- Commands: `NetworkCommand` enum (`uint8_t`) interpreted from the low 8 bits of `IORequest.command`

Command IDs (v1):

| Command | ID | Purpose |
|--------:|---:|---------|
| `Open`  | `0x01` | Create a request/session handle |
| `Read`  | `0x02` | Read response bytes at offset |
| `Write` | `0x03` | Write request body bytes at offset |
| `Close` | `0x04` | Release a handle |
| `Info`  | `0x05` | Fetch response metadata (HTTP status, headers, length) |

v1 goal: support streaming reads without buffering entire bodies in RAM.

---

## Common Response Prefix

Many commands return this prefix:

```
u8   version            // = 1
u8   flags              // command-specific
u16  reserved           // = 0 for now (LE)
```

Reserved is for forward-compatible expansion.

---

## Method Encoding

The `Open` command uses a compact method code:

| Method | Code |
|-------:|-----:|
| GET    | 1 |
| POST   | 2 |
| PUT    | 3 |
| DELETE | 4 |
| HEAD   | 5 |

---

## Command: Open (0x01)

Create a session handle for a URL. `Open` is allowed to be asynchronous: it may return immediately and the session becomes readable later (via `poll()`).

### Request

```
u8   version
u8   method              // 1..5
u8   flags               // see below
u16  urlLen              // LE
u8[] url                 // length urlLen
u16  headerCount         // LE
repeat headerCount times:
  u16 keyLen             // LE
  u8[] key               // length keyLen
  u16 valLen             // LE
  u8[] value             // length valLen
u32  bodyLenHint         // LE; 0 if unknown
```

### Open flags (u8)
bit0 = tls
bit1 = follow_redirects
bit2 = want_headers
bit3 = allow_evict (NEW; v1.0 compatible)

allow_evict semantics:
- When allow_evict=0 (default), `Open` MUST return `DeviceBusy` if no free handles are available.
  The device MUST NOT evict an existing active handle.
- When allow_evict=1, the device MAY evict the least-recently-used (LRU) active handle to satisfy the Open.
  Any evicted handle becomes immediately invalid; subsequent Info/Read/Write/Close on it MUST return InvalidRequest.

### Response

```
u8   version
u8   flags               // bit0=accepted, bit1=needs_body_write
u16  reserved            // = 0
u16  handle              // LE
```

### Status codes

- `Ok`: handle allocated (even if network work is not complete yet)
- `DeviceBusy`: no free handles
- `NotReady`: network unavailable
- `InvalidRequest`: malformed payload
- `InternalError`: unexpected failure

Notes:
- `needs_body_write=1` indicates the host must stream body via `Write` (POST/PUT).
- `accepted=1` means the handle exists; it does not imply the request has completed.

### Headers

- Headers are only collected if the OPEN flag `want_headers` is set.
- POSIX backends may capture all response headers and return up to max_header_bytes.
- ESP32 backends should avoid storing full headers unless requested, and should prefer filtering/storing only what the client asked for (future enhancement).

---

## Command: Write (0x03)

Stream request body bytes into an open handle (for POST/PUT). Chunking is host-driven.

### Request

```
u8   version
u16  handle              // LE
u32  offset              // LE
u16  dataLen             // LE
u8[] data                // length dataLen
```

### Response

```
u8   version
u8   flags               // reserved; 0 for now
u16  reserved            // = 0
u16  handle              // LE
u32  offsetEcho          // LE
u16  writtenLen          // LE
```

### Status codes

- `Ok`
- `InvalidRequest` (bad handle, missing bytes, etc.)
- `IOError` (write failed)
- `DeviceBusy` (backpressure / buffer full; host should retry)

---

## Command: Read (0x02)

Read response body bytes at an explicit offset. Chunking is host-driven.

### Request

```
u8   version
u16  handle              // LE
u32  offset              // LE
u16  maxBytes            // LE; requested bytes to read
```

### Response

```
u8   version
u8   flags               // bit0=eof, bit1=truncated
u16  reserved            // = 0
u16  handle              // LE
u32  offsetEcho          // LE
u16  dataLen             // LE; actual bytes returned
u8[] data                // length dataLen
```

### Status codes

- `Ok` (possibly `dataLen=0`)
- `NotReady` (response not ready yet; host should retry later)
- `InvalidRequest` (bad handle or malformed payload)
- `IOError` (read failed)

Notes:
- `eof=1` indicates end-of-response reached.
- truncated: set when the device filled the requested max_bytes (i.e. caller buffer limit hit).
  It does NOT mean "server truncated the response".
- The host reads until `eof=1` or `dataLen=0`.

#### READ behavior notes (important)

Backends may be synchronous (POSIX curl) or streaming/asynchronous (ESP32).

- If data is not yet available, READ may return `NotReady`.
- A READ returning `Ok` with `read_len == 0` is only valid when `eof == true` (transfer complete).
- Hosts should treat `NotReady` as "try again soon", not a fatal error.

---

## Command: Info (0x05)

Fetch response metadata (HTTP status, content length, optional headers). This can be called repeatedly; results may become available over time.

- content_length is optional:
  - If the backend knows it (e.g., Content-Length header), it reports it.
  - For chunked/streaming responses, it may be 0/absent until completion or unknown entirely.

### Request

```
u8   version
u16  handle              // LE
u16  maxHeaderBytes      // LE; max bytes of headers to return (0 allowed)
```

### Response

```
u8   version
u8   flags               // bit0=headersIncluded, bit1=hasContentLength, bit2=hasHttpStatus
u16  reserved            // = 0
u16  handle              // LE
u16  httpStatus          // LE; valid only if hasHttpStatus
u64  contentLength       // LE; valid only if hasContentLength
u16  headerBytesLen      // LE; number of header bytes returned
u8[] headerBytes         // raw "Key: Value\r\n" bytes; may be truncated
```

### Status codes

- `Ok`
- `NotReady` (metadata not available yet)
- `InvalidRequest`
- `IOError`

Notes:
- `headerBytes` is intentionally unstructured to keep parsing simple for 8-bit hosts.
- Modern tooling can parse it easily.

---

## Command: Close (0x04)

Release resources for a handle.

### Request

```
u8   version
u16  handle              // LE
```

### Response

Optional minimal response payload:

```
u8  version
u8  flags=0
u16 reserved=0
```

### Status codes

- `Ok`
- `InvalidRequest` (unknown handle)

---

## Chunking Responsibility

Chunking is **host-driven**:

- Host repeatedly calls `Read`/`Write` with increasing offsets.
- Device does **no unsolicited continuation**.
- Device may return `DeviceBusy` to apply backpressure (host retries).

This model scales to:
- large HTTP downloads
- disk images fetched over HTTP
- constrained RAM platforms

---

## Implementation Notes (Device)

### Session table
NetworkDevice should keep a fixed-size table of sessions (small number, e.g. 4–8):

- active flag
- method/url/headers
- body-written tracking
- response-ready tracking
- HTTP status, content length, headers buffer
- response byte source (stream/buffer)
- eof flag
- timeouts / last activity

### poll()
`NetworkDevice::poll()` should:
- advance in-flight sessions
- perform network I/O
- fill response buffers progressively
- update NotReady vs Ok outcomes for `Read`/`Info`

---

### Handle

`handle` is a 16-bit opaque session identifier. Internally it encodes:
- a small session slot index (bounded by MAX_SESSIONS), and
- a generation counter to detect stale handles after a slot is reused.

Implications:
- Handle values are not sequential, and may look like multiples of 256, etc.
- Max concurrent sessions is bounded by MAX_SESSIONS; OPEN returns DeviceBusy when full.
- A handle may become invalid after CLOSE (or timeout reaping), even if the numeric value is later reused with a new generation.

---

## Future Extensions (Non-breaking)

The version byte and reserved fields allow future expansion:

- authentication helpers
- stable header enumeration via cookies/handles
- websocket or raw socket support
- negotiated maximum payload sizes per transport
- streaming compression flags
