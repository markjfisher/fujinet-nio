# Network TODOs (next iteration)

## 0) Baseline stability + parity
- [ ] Verify POSIX (curl) and ESP32 (ESP-IDF) return the same StatusCode for the same situations:
      - open(): bad URL / unsupported scheme / DNS fail / connect fail / TLS fail / redirect fail
      - info(): before ready, after ready, after EOF, after close
      - read(): NotReady vs Ok(0 bytes) vs IOError, and EOF signalling consistency
      - close(): idempotency and handling of unknown/expired handles
- [ ] Define a single “contract” table: {operation × condition → StatusCode} and align both backends to it.
- [ ] Add minimal cross-platform tests for NetworkDevice session semantics (slot allocation, handle generation, timeouts, close behaviour).

## 1) HTTP request body support (POST/PUT) + method surface
- [ ] Implement POST and PUT end-to-end:
      - open(): accept bodyLenHint, return needs_body_write=true when appropriate
      - write_body(): accept sequential offset writes and stream to backend
      - finalize/send: define when request is actually dispatched (on first read/info? explicit “commit”? when bodyLenHint reached?)
- [ ] Implement DELETE (allow optional body).
- [ ] Decide and document: “v1 sequential offsets only” for write_body/read_body (no random access) and enforce consistently.

## 2) HTTP headers: move from “max bytes” to “requested header keys”
- [ ] Introduce protocol change (v2) to let the client request specific response headers by name:
      - open request includes list of desired response headers (case-insensitive match)
      - backend stores only those headers (plus minimal required metadata)
      - info() returns only requested headers (or a structured result)
- [ ] Decide behaviour when headers are not requested:
      - info() still returns http_status and content_length (if available)
      - header block empty and headersIncluded=false
- [ ] Keep backward compatibility strategy:
      - v1: maxHeaderBytes continues to work
      - v2: preferred header filtering API
      - document negotiation and version handling clearly

## 3) HTTP streaming correctness + performance
- [ ] Ensure ESP32 HTTP read semantics are deterministic:
      - first READ should set eof=true when the transfer is already complete and no more bytes are pending
- [ ] Confirm ESP32 content_length behaviour:
      - when unknown (chunked), report “hasContentLength=false”
      - when known, return the actual value
- [ ] Decide how NotReady should be used:
      - async transfers: NotReady means “try again later”
      - sync transfers: avoid NotReady unless absolutely necessary
- [ ] Tune timeouts and remove host-side “timeout-driven polling” where possible (use retry-on-NotReady with a short sleep/backoff).

## 4) TCP protocol backend (stream sockets)
- [ ] Define scheme and URL form for TCP:
      - e.g. tcp://host:port (and possibly tls://host:port later)
- [ ] Implement TCP backend on POSIX:
      - non-blocking socket or blocking with sane timeouts
      - read_body streams bytes; write_body sends bytes
      - info() provides connected/errored and bytes-available if you can support it
- [ ] Implement TCP backend on ESP32:
      - lwIP sockets
      - ring/stream buffer for RX, with backpressure
      - write_body sends immediately (or buffered) with sequential offsets
- [ ] Decide how to support “bytes waiting”:
      - expose via info() (v1) or a dedicated “status” command in v2
      - define meaning precisely for both HTTP and TCP:
          - bytes waiting right now in RX buffer
          - connection alive flag
          - last error code presence

## 5) Session management rules (bad clients / legacy behaviour)
- [ ] Decide policy for “open on an already-open handle/slot”:
      - strict (return DeviceBusy)
      - or legacy-compatible (close existing and reopen)
      - if supporting both, gate it behind a config flag and document it
- [ ] Ensure idle-timeout + max-lifetime behaviour is consistent across platforms.
- [ ] Add optional diagnostics logging / lightweight debug command support (probably via FujiDevice) to dump session table:
      - active handles, scheme, state, bytes available, done/error flags, age/idle ticks

## 6) Documentation updates
- [ ] Update docs/network_device_protocol.md:
      - include current schemes, v1 semantics, and planned v2 header filtering + status/bytes-waiting support
      - clarify EOF rules and NotReady usage
- [ ] Update docs/architecture.md:
      - include the networking section (registry/backends/sessions/polling hook) and explicitly note which parts are hooks vs required today.

## 7) Tooling (Python CLI) hardening
- [ ] Make net.py behave like a real host client:
      - retry NotReady with short backoff
      - stop reading once eof=true OR (read==0 and eof==true)
      - do not rely on long serial timeouts as flow control
- [ ] Add status text mapping everywhere (already partially present) and print StatusCode names in errors consistently.
- [ ] Add a small “net soak” script that opens N sessions, interleaves reads, and verifies correctness + timing.
