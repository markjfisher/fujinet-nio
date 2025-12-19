# NetworkDevice TCP Protocol Semantics (v1)

This document defines how TCP stream sockets are implemented on top of the
existing NetworkDevice v1 binary protocol.

The TCP design fits entirely within the existing v1 command set:
- Open
- Read
- Write
- Info
- Close

No new wire commands or payload formats are introduced.

---

## 1. Overview

The NetworkDevice TCP backend provides access to TCP stream sockets using URLs
of the form:

    tcp://host:port[?options]

TCP is a **stream-oriented, non-seekable** resource. As such, it differs
significantly from file-based protocols and HTTP, but is still compatible
with the v1 protocol by interpreting offsets and Info metadata differently.

The same application-level protocol runs unchanged on:
- POSIX (BSD sockets)
- ESP32 (lwIP sockets)

---

## 2. URL Scheme

### Base form

    tcp://host:port

- `host` may be a hostname, IPv4 address, or IPv6 address
  (IPv6 must be bracketed: `tcp://[::1]:1234`)
- `port` is required

### Query options

All options are optional.

| Option | Meaning | Default |
|------|--------|---------|
| `connect_timeout_ms` | Timeout for TCP connect | 5000 |
| `nodelay` | Enable TCP_NODELAY | 1 |
| `keepalive` | Enable SO_KEEPALIVE | 0 |
| `rx_buf` | Receive buffer size (bytes) | 8192 |
| `halfclose` | Allow shutdown(SHUT_WR) via zero-length Write | 1 |

Unknown options are ignored to preserve forward compatibility.

---

## 3. Open Semantics

`Open()` for TCP:

- Allocates a NetworkDevice handle.
- Begins a non-blocking TCP connect.
- Returns `StatusCode::Ok` once the connect attempt has started.
- Does **not** require a body write phase.

Notes:
- The HTTP `method` field is ignored for TCP.
- `bodyLenHint` has no semantic meaning for TCP.

Possible status codes:
- `Ok` – connect started or completed
- `NotReady` – network stack unavailable
- `IOError` – DNS failure, immediate connect failure, or timeout
- `InvalidRequest` – malformed URL

---

## 4. Stream Offsets (Critical Concept)

TCP streams have no natural offsets. Instead, NetworkDevice offsets are treated
as **monotonic stream cursors**.

Each TCP handle maintains:
- `read_cursor`: total bytes delivered to the host
- `write_cursor`: total bytes accepted for sending

### Offset rules

For both `Read` and `Write`:

    offset MUST equal the current cursor

If `offset != expected_cursor`, the backend returns:

    StatusCode::InvalidRequest

This rule ensures deterministic, ordered stream access and prevents accidental
rewinds or replays on non-seekable resources.

---

## 5. Write Semantics (send)

`Write(handle, offset, data)` maps to `send()`.

Rules:
- `offset` must equal `write_cursor`
- Partial writes are allowed
- `write_cursor` advances by the number of bytes successfully written

Return behavior:
- `Ok` – one or more bytes written
- `DeviceBusy` – send would block (retry same offset)
- `NotReady` – connection not yet established
- `IOError` – connection error
- `InvalidRequest` – offset mismatch

### Half-close (optional)

If `halfclose=1`, a zero-length write:

    Write(handle, write_cursor, len=0)

causes the backend to call `shutdown(SHUT_WR)`, signaling end-of-stream
to the peer while still allowing reads.

---

## 6. Read Semantics (recv)

`Read(handle, offset, maxBytes)` returns bytes from the TCP stream.

Rules:
- `offset` must equal `read_cursor`
- Reads are non-blocking
- Data may come from an internal receive buffer

Return behavior:
- `Ok` with `dataLen > 0` – bytes returned
- `NotReady` – no data available yet
- `Ok` with `dataLen = 0` and `eof = 1` – peer closed and buffer drained
- `IOError` – socket error
- `InvalidRequest` – offset mismatch

EOF is only reported **after** the peer has closed and all buffered data
has been consumed.

---

## 7. Info() Semantics for TCP

The `Info` command uses the existing v1 payload format, but with
TCP-specific interpretation.

### Standard fields

For TCP:
- `hasHttpStatus = 0`
- `hasContentLength = 0`
- `httpStatus = 0`
- `contentLength = 0`

These fields are unused because TCP is not HTTP.

### Pseudo headers

When `maxHeaderBytes > 0`, the backend returns ASCII key/value pairs
in `headersBlock`, similar to HTTP headers but protocol-defined.

Example:

    X-FujiNet-Scheme: tcp
    X-FujiNet-Remote: example.com:1234
    X-FujiNet-Connecting: 0
    X-FujiNet-Connected: 1
    X-FujiNet-PeerClosed: 0
    X-FujiNet-RxAvailable: 128
    X-FujiNet-ReadCursor: 1024
    X-FujiNet-WriteCursor: 512
    X-FujiNet-LastError: 0

These headers allow clients to inspect TCP state without extending the
v1 wire format.

Headers may be truncated if they exceed `maxHeaderBytes`.

---

## 8. Comparison with HTTP

| Aspect | HTTP | TCP |
|------|-----|-----|
| Resource type | Request/response | Bidirectional stream |
| Seekable | No | No |
| Offsets | Stream cursor | Stream cursor |
| Content length | Known or chunked | Unknown |
| EOF | End of body | Peer close |
| Info headers | HTTP headers | Pseudo headers |
| Status code | HTTP status | Not applicable |

Both protocols reuse the same v1 commands and payload layouts but interpret
them differently.

---

## 9. Platform Notes

- POSIX: implemented using non-blocking BSD sockets
- ESP32: implemented using lwIP BSD socket compatibility layer
- No `#ifdef` logic exists in core protocol code
- Platform differences are isolated to backend implementations

---

## 10. Compatibility and Versioning

The TCP protocol is fully compatible with NetworkDevice v1:
- No new commands
- No new payload fields
- No changes to existing wire formats

Future extensions (e.g. TLS via `tls://`) can be introduced as new schemes
without breaking v1 compatibility.

