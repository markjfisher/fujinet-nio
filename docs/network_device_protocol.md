# NetworkDevice Binary Protocol

This document specifies the **binary, non-JSON** request/response format used by the **NetworkDevice** (`WireDeviceId::NetworkService`, proposed `0xFD`).

Design goals:

- **8-bit-host friendly** (no JSON, minimal parsing)
- **Transport agnostic** (FujiBus+SLIP today; compatible with future transports)
- **Streaming and chunkable** (arbitrary response sizes via explicit offsets)
- **Poll-driven** (device progresses network work in `poll()`; host pulls results)
- **Explicit state** (network operations are represented by handles)

This protocol sits **above** any HTTP/TLS implementation and **above** any socket stack. It is purely a device protocol.

This document describes the v1 NetworkDevice binary protocol and its command
semantics (Open / Read / Write / Info / Close).

The protocol is transport-agnostic: multiple network schemes may be implemented
over it. At present, two schemes exist:

- HTTP/HTTPS (request–response semantics)
- TCP (stream socket semantics)

HTTP behavior is documented inline below.
TCP stream behavior is documented separately in:

  docs/network_device_tcp.md

Unless explicitly stated otherwise, command semantics described here apply to
both HTTP and TCP. Where semantics differ (e.g. offsets, EOF behavior, Info()),
the TCP document is authoritative.

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

## Transport Framing (Non-normative)

This protocol is agnostic to the underlying transport framing (e.g. SLIP, USB CDC, UART buffers).

- Hosts MUST NOT assume that a single Read or Write maps to a single transport frame.
- Devices MUST present protocol-correct responses independent of transport buffering.
- Any transport-imposed size limits are handled by host-driven chunking, not by the protocol.


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
u8   method            // 1..5
u8   flags             // see below
u16  urlLen            // LE
u8[] url               // length urlLen

u16  headerCount       // LE

repeat headerCount times:
  u16  keyLen          // LE
  u8[] key             // length keyLen
  u16  valLen          // LE
  u8[] value           // length valLen

u32  bodyLenHint       // LE; 0 if unknown

u16  respHeaderCount   // LE; number of response header names to capture

repeat respHeaderCount times:
  u16  nameLen         // LE
  u8[] name            // length nameLen (header name, ASCII; case-insensitive)
```

### Open flags (u8)
bit0 = tls
bit1 = follow_redirects
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

### Response header capture (v1)

- The client provides a list of response header **names** in `respHeaderCount`.
- The device/backend stores **only** those headers (case-insensitive match).
- If `respHeaderCount == 0`, the device stores **no** response headers.
- Devices MUST NOT add implicit HTTP headers (e.g. Content-Type) unless explicitly requested by the host via headers in the Open request.


---

## Command: Write (0x03)

Stream request body bytes into an open handle (for POST/PUT) **or** stream bytes into a non-HTTP protocol (e.g. TCP).
Chunking is host-driven.

### Request
```
u8 version
u16 handle        // LE
u32 offset        // LE
u16 dataLen       // LE
u8[] data         // length dataLen
```

### Response
```
u8 version
u8 flags          // reserved; 0 for now
u16 reserved      // = 0
u16 handle        // LE
u32 offsetEcho    // LE
u16 writtenLen    // LE
```

### Status codes
- `Ok`
- `InvalidRequest` (bad handle, missing bytes, etc.)
- `IOError` (write failed)
- `DeviceBusy` (backpressure / buffer full; host should retry)

### Offset semantics (important)

The meaning of `offset` depends on the protocol bound to the handle:

- **HTTP request bodies (http/https)**:
  - Offsets MUST be sequential (0..N) and MUST NOT exceed `bodyLenHint`.
  - See `## HTTP request lifecycle (HTTP/HTTPS schemes)` for the authoritative commit rule.

- **Stream protocols (tcp)**:
  - Offsets represent a **monotonic write cursor** (bytes accepted so far).
  - The device/backend MUST require strictly sequential offsets:
    - If `offset` is a rewind or gap, return `InvalidRequest`.
  - Partial writes are allowed; `writtenLen` may be less than `dataLen`.
  - If the stream is not yet connected, the backend may return `NotReady`.

### TCP half-close (optional)

For TCP handles, a zero-length write MAY be used as a send-finish signal:

- `Write(handle, offset=write_cursor, dataLen=0)` MAY cause the backend to call `shutdown(SHUT_WR)`
  (if enabled by backend configuration such as `halfclose=1` in the URL).

### Offset semantics (scheme-dependent)

As with Read, the meaning of `offset` depends on the scheme:

- For HTTP/HTTPS:
  - Writes append to the outgoing request body.
  - Offset is typically ignored or must be zero.

- For TCP:
  - `offset` is a sequential stream cursor.
  - Writes MUST be strictly sequential.
  - A Write request whose `offset` does not match the current stream write
    cursor MUST fail with StatusCode::InvalidRequest.

### Zero-length write (TCP half-close)

For the TCP scheme, a Write request with `length == 0` MAY be used to signal
end-of-stream (half-close), if enabled by the URL options. This maps to a
shutdown of the socket write side (e.g. `shutdown(SHUT_WR)`).

See docs/network_device_tcp.md for details.

---

## Command: Read (0x02)

Read response body bytes at an explicit offset. Chunking is host-driven.

### Request
```
u8 version
u16 handle     // LE
u32 offset     // LE
u16 maxBytes   // LE; requested bytes to read
```

### Response
```
u8 version
u8 flags       // bit0=eof, bit1=truncated
u16 reserved   // = 0
u16 handle     // LE
u32 offsetEcho // LE
u16 dataLen    // LE; actual bytes returned
u8[] data      // length dataLen
```

### Status codes
- `Ok` (possibly `dataLen=0`)
- `NotReady` (response not ready yet; host should retry later)
- `InvalidRequest` (bad handle or malformed payload)
- `IOError` (read failed)

Notes:
- `eof=1` indicates end-of-response reached.
- `truncated`: set when the device filled the requested max_bytes (i.e. caller buffer limit hit).
  It does NOT mean "server truncated the response".
- The host reads until `eof=1` or `dataLen=0`.

### Offset semantics (important)

The meaning of `offset` depends on the protocol bound to the handle:

- **HTTP response bodies (http/https)**:
  - Offset represents the byte position within the response body stream as returned to the host.
  - The host MUST request subsequent chunks using increasing offsets.

- **Stream protocols (tcp)**:
  - Offset represents a **monotonic read cursor** (bytes delivered so far).
  - The device/backend MUST require strictly sequential offsets:
    - If `offset` is a rewind or gap, return `InvalidRequest`.

### Offset semantics (scheme-dependent)

The meaning of the `offset` field depends on the active network scheme:

- For HTTP/HTTPS:
  - `offset` refers to a byte offset within the response body.
  - Random-access reads MAY be supported by the backend (e.g. via range
    requests), but are not guaranteed.

- For TCP:
  - `offset` is a sequential stream cursor.
  - Reads MUST be strictly sequential.
  - A Read request whose `offset` does not match the current stream read
    cursor MUST fail with StatusCode::InvalidRequest.

See docs/network_device_tcp.md for full TCP stream semantics.

#### READ behavior notes (important)

Backends may be synchronous (POSIX curl) or streaming/asynchronous (ESP32, TCP).

- If data is not yet available, READ may return `NotReady`.
- A READ returning `Ok` with `read_len == 0` is only valid when `eof == true` (transfer complete / peer closed).
- Hosts should treat `NotReady` as "try again soon", not a fatal error.
- Transport-layer framing limits (e.g. USB, SLIP, CDC buffers) MUST NOT affect protocol semantics.
  Hosts should assume that large responses require multiple Read calls.

#### Read size guarantees
- `dataLen` in the response MUST be `<= maxBytes` from the request.
- The device MUST NOT return more than `maxBytes` of data in a single Read response.
- If more data is available, the host MUST issue subsequent Read requests with an updated offset.

---

## Command: Info (0x05)

Fetch response metadata (HTTP status, content length, optional headers) **or** protocol-defined metadata for non-HTTP schemes.
This can be called repeatedly; results may become available over time.

- content_length is optional:
  - If the backend knows it (e.g., Content-Length header), it reports it.
  - For chunked/streaming responses, it may be 0/absent until completion or unknown entirely.

### Request
```
u8 version
u16 handle          // LE
```

### Response
```
u8 version
u8 flags            // bit0=headersIncluded, bit1=hasContentLength, bit2=hasHttpStatus
u16 reserved        // = 0
u16 handle          // LE
u16 httpStatus      // LE; valid only if hasHttpStatus
u64 contentLength   // LE; valid only if hasContentLength
u16 headerBytesLen  // LE; number of header bytes returned
u8[] headerBytes    // raw "Key: Value\r\n" bytes (only requested headers)
```

### Status codes
- `Ok`
- `NotReady` (metadata not available yet)
- `InvalidRequest`
- `IOError`

Notes:
- `headerBytes` is intentionally unstructured to keep parsing simple for 8-bit hosts.
- Modern tooling can parse it easily.

### Protocol-specific interpretation

The Info wire format is stable, but interpretation is scheme-specific:

- **HTTP/HTTPS**:
  - `hasHttpStatus=1` when HTTP status is known, and `httpStatus` carries the HTTP status code.
  - `hasContentLength=1` when the response length is known.
  - `headerBytes` contains HTTP response headers ("Key: Value\r\n").

- **TCP (tcp://)**:
  - `hasHttpStatus=0` and `hasContentLength=0`.
  - `headerBytes` (if requested) contains **pseudo headers** that expose TCP state and counters
    without changing the v1 wire format.
  - See `docs/network_device_tcp.md` for the pseudo header keys and semantics.

### Info / Read ordering
- `Info()` and `Read()` are independent.
- `Info()` MAY return `Ok` before any body data is readable.
- `Read()` MAY return `NotReady` even after `Info()` succeeds.
- Hosts MUST treat both commands as independently retryable.

### TCP scheme behavior

For the TCP scheme:

- HTTP-specific fields (httpStatus, contentLength) are not meaningful and are
  reported as absent.
- Connection state and stream status are exposed via pseudo-headers in the
  headers block.

Implementations MAY include the following headers:

- X-FujiNet-Scheme: tcp
- X-FujiNet-Remote: host:port
- X-FujiNet-Connecting: 0|1
- X-FujiNet-Connected: 0|1
- X-FujiNet-PeerClosed: 0|1
- X-FujiNet-RxAvailable: <bytes currently buffered>
- X-FujiNet-ReadCursor: <current stream read offset>
- X-FujiNet-WriteCursor: <current stream write offset>
- X-FujiNet-LastError: <platform errno value or 0>

These headers allow TCP connection state to be queried without breaking v1
compatibility.

See docs/network_device_tcp.md for authoritative TCP definitions.

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

## HTTP request lifecycle (HTTP/HTTPS schemes)

### Overview

HTTP requests are created by `Open()`.

If the request has a body (POST/PUT and optional others), the body is streamed via `Write()` calls.
The request is dispatched automatically when the body is complete (or immediately for no-body requests).

Response data is retrieved with `Read()`.

### Body-required methods

For HTTP:
- POST and PUT MAY require a body (depending on `bodyLenHint`).
- GET/HEAD/DELETE typically have no body (we do not rely on a body for these in v1).

### Dispatch / commit rule (authoritative)

- If `bodyLenHint == 0`:
  - The request MUST be dispatched immediately during `Open()`.

- If `bodyLenHint > 0`:
  - `Open()` MUST return `needs_body_write=1` in the Open response flags.
  - The client MUST send body bytes via one or more `Write(handle, offset, bytes)` calls.
  - The request MUST be dispatched automatically once the device has received exactly `bodyLenHint` bytes total for that handle.
  - `Read()` / `Info()` before dispatch completion MUST return `NotReady` (unless an error occurs).

### Write() rules for HTTP body streaming

- Offsets:
  - Offsets MUST be sequential (0..N) for HTTP request bodies.
  - If a `Write()` offset is non-sequential (gap or rewind), the device MUST return `InvalidRequest`.

- Length:
  - Total bytes written MUST NOT exceed `bodyLenHint`.
    If it would exceed, the device MUST return `InvalidRequest`.

- Backpressure:
  - If the device cannot accept more body bytes right now, `Write()` MAY return `DeviceBusy`.

### Errors during upload / dispatch

If the body upload fails or dispatch fails, subsequent `Info/Read` MUST return an appropriate error
(typically `IOError`) and the handle remains closeable.

### Contrast: TCP stream lifecycle (tcp scheme)

TCP uses the same v1 commands (Open/Write/Read/Info/Close) but differs from HTTP:

- There is no HTTP method or request/response concept.
- `Write()` maps to streaming `send()` and is not gated by `needs_body_write`.
- `Read()` maps to streaming `recv()`.
- Offsets are still required by the v1 protocol, but for TCP they are interpreted as
  **monotonic stream cursors** (sequential offsets only).
- `Info()` uses `headerBytes` for **pseudo headers** that expose TCP state (connected, bytes available, last error, etc.).

See `docs/network_device_tcp.md` for the full TCP mapping and pseudo header keys.

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

## Protocol-specific extensions

The NetworkDevice protocol is designed to support multiple network
protocols using the same command set and wire formats.

Protocol-specific behavior is documented separately:

- **TCP stream sockets**: see `docs/network_device_tcp.md`

These documents describe how each protocol maps its semantics onto the
core Open / Read / Write / Info / Close commands.
