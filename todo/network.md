# Network TODOs (next iteration)

## 0) Baseline stability + parity

### Goal (definition of done)
- POSIX (curl) and ESP32 (ESP-IDF) return the same `StatusCode` for the same conditions, across:
  - `Open`, `Info`, `Read`, `Write`, `Close`
- NetworkDevice session rules are deterministic:
  - handle generation prevents stale reuse issues
  - idle-timeout reaping works the same across platforms
  - `Close` behaviour is clearly defined (including unknown/expired handles)
- Current status: functional parity for HTTP request/response flows validated manually (ESP32 + POSIX), but contract matrix + automated tests still need adding before formal sign-off.

### Session capacity + mapping policy (MUST be explicit)

- NetworkDevice maintains a finite pool of concurrent sessions (currently 4).
- Mapping from legacy logical network devices (e.g. Atari N1..N8 / "n1:" prefixes) to NetworkDevice handles
  is owned by the transport personality (e.g. LegacyAtariTransport), not by NetworkDevice itself.
  - On re-open of the same logical unit (e.g. "n1"), the transport SHOULD best-effort Close the previously
    mapped handle and then Open the new URL, updating the mapping.
  - Legacy transport MUST prevent more than 4 concurrent open logical units; attempting to open a 5th
    MUST fail at the transport/library level (before it reaches NetworkDevice).

- NetworkDevice `Open` capacity behaviour is controlled by an Open flag:
  - `allow_evict = 0` (default): if the session pool is full, `Open` MUST return `DeviceBusy`.
  - `allow_evict = 1`: if the session pool is full, `Open` MAY evict an LRU active session to satisfy the Open.
    - Any evicted handle becomes immediately invalid; subsequent Info/Read/Write/Close on it MUST return `InvalidRequest`.

Rationale:
- Preserves backwards compatibility for legacy host apps (transport-managed logical devices).
- Allows tool / direct-handle clients to opt into eviction for convenience if desired.


### Canonical StatusCode Contract Matrix (MUST be kept up-to-date)
> This table is the source of truth. If a backend disagrees, the backend changes (or the table changes).
> Keep the contract aligned with current NetworkDevice behaviour unless we intentionally decide otherwise.

#### Open()
| Condition | Expected StatusCode | Notes |
|---|---:|---|
| payload malformed / wrong version / trailing bytes | InvalidRequest | protocol decode failure |
| URL missing scheme / scheme parse fails | InvalidRequest | e.g. no `://` |
| scheme unsupported / no backend registered | Unsupported | e.g. tcp not implemented yet |
| no free session slots AND allow_evict=0 | DeviceBusy | strict capacity |
| no free session slots AND allow_evict=1 | Ok (after LRU eviction) OR DeviceBusy | DeviceBusy only if no eviction possible |
| network stack unavailable / WiFi down (platform-specific) | NotReady | *only* when truly "try later" |
| DNS failure | IOError | retryable at host discretion |
| connect failure / timeout | IOError | |
| TLS negotiation / cert failure | IOError | |
| redirect failure (when follow_redirects enabled) | IOError | or Unsupported (only if we intentionally define it that way) |
| backend internal unexpected failure | InternalError | |

#### Info(handle)
| Condition | Expected StatusCode | Notes |
|---|---:|---|
| payload malformed / wrong version / trailing bytes | InvalidRequest | |
| handle unknown/expired | InvalidRequest | align to current NetworkDevice behaviour |
| handle valid but response metadata not available yet | NotReady | host should retry |
| info available | Ok | flags determine what fields are valid |
| backend error after open (e.g. connection drop) | IOError | |

#### Read(handle, offset, maxBytes)
| Condition | Expected StatusCode | Notes |
|---|---:|---|
| payload malformed / wrong version / trailing bytes | InvalidRequest | |
| handle unknown/expired | InvalidRequest | align to current NetworkDevice behaviour |
| response not ready yet / no bytes currently available | NotReady | async: common, sync: rare |
| read returns bytes | Ok | dataLen > 0 |
| transfer complete and no more bytes | Ok | dataLen == 0 AND eof == true |
| any read failure after open | IOError | |

#### Write(handle, offset, bytes)
| Condition | Expected StatusCode | Notes |
|---|---:|---|
| payload malformed / wrong version / missing bytes / trailing bytes | InvalidRequest | |
| handle unknown/expired | InvalidRequest | align to current NetworkDevice behaviour |
| method/body not supported by backend | Unsupported | e.g. POST/PUT not yet implemented |
| backpressure / buffer full | DeviceBusy | host retries |
| write accepted | Ok | returns writtenLen |
| write failure | IOError | |

#### Close(handle)
| Condition | Expected StatusCode | Notes |
|---|---:|---|
| payload malformed / wrong version / trailing bytes | InvalidRequest | |
| handle unknown/expired | InvalidRequest | (explicitly non-idempotent today) |
| close ok | Ok | frees slot immediately |

### Cross-platform conformance tests (minimal set) (REQUIRED BEFORE SIGN OFF)
- Add tests that validate BOTH:
  1) NetworkDevice session semantics (platform-agnostic)
  2) Backend-mapped error/status semantics (POSIX vs ESP32 parity where feasible)

#### (A) Session semantics tests (platform-agnostic)
- handle generation:
  - Open → Close → Open in same slot must produce a different handle generation (stale handle rejected)
- unknown handle behaviour:
  - Info/Read/Write/Close on unknown handle → InvalidRequest
- slot allocation:
  - when allow_evict=0:
    - Open MAX_SESSIONS times succeeds, next Open → DeviceBusy
  - when allow_evict=1:
    - Open beyond MAX_SESSIONS succeeds by evicting LRU
    - evicted handles become InvalidRequest for Info/Read/Write/Close
- idle timeout:
  - simulate ticks; session idle reaped; subsequent Info/Read/Close → InvalidRequest

#### (B) Backend parity tests (POSIX/ESP32 where possible)
- Open with unsupported scheme → Unsupported
- Open with malformed URL (no scheme) → InvalidRequest
- Read before ready:
  - either NotReady (preferred) OR Ok+eof (only if completed immediately); must be consistent per backend rules
- Info before ready:
  - NotReady, then Ok once status becomes available

### Touchpoints (expected files)
- Device semantics: `src/lib/network_device.cpp`
- Protocol contract reference: `docs/network_device_protocol.md`
- POSIX backend mapping: `src/platform/posix/http_network_protocol_curl.cpp`
- ESP32 backend mapping: `src/platform/esp32/http_network_protocol_espidf.cpp`
- Tests: `tests/test_network_device_protocol.cpp` (or a new `tests/test_network_device_parity.cpp`)


## 1) HTTP verbs + request body lifecycle (POSIX + ESP32 parity)

### Goal (definition of done)
- HTTP backends support: GET, HEAD, POST, PUT, DELETE.
- Request body streaming works end-to-end for POST/PUT:
  - `Open(bodyLenHint>0)` -> `needs_body_write=1`
  - sequential `Write()` uploads body
  - auto-dispatch when `bodyLenHint` bytes received
  - `Info/Read` are `NotReady` until dispatched (or error)
- POSIX and ESP32 return aligned StatusCode results for the same scenarios.

### Tasks
- [x] Update protocol doc: HTTP request lifecycle + dispatch/commit rule (see above).
- [x] NetworkDevice: enforce body streaming rules (sequential offsets, no overflow beyond hint).
- [x] POSIX (curl):
  - [x] extend method support: POST/PUT/DELETE
  - [x] implement write_body() for buffering/streaming request body
  - [x] ensure dispatch occurs at correct time (Open immediately if no-body, otherwise after body complete)
- [x] ESP32 (esp-idf http client):
  - [x] extend method support: POST/PUT/DELETE
  - [x] implement body upload + dispatch timing to match POSIX
  - [x] fix USB/CDC write behaviour so larger responses can be read reliably
- [x] Tests (POSIX):
  - [x] unit tests for body rules (non-sequential offset => InvalidRequest; overflow => InvalidRequest)
  - [x] unit tests that POST/PUT with bodyLenHint>0 return needs_body_write
  - [x] unit tests that Read/Info before dispatch return NotReady

### Touchpoints
- `src/lib/network_device.cpp`
- `src/platform/posix/http_network_protocol_curl.cpp`
- `src/platform/esp32/http_network_protocol_espidf.cpp`
- `docs/network_device_protocol.md`
- `tests/test_network_device_protocol.cpp`


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
- [x] Define scheme and URL form for TCP:
      - e.g. tcp://host:port (and possibly tls://host:port later)
- [x] Implement TCP backend on POSIX:
      - non-blocking socket or blocking with sane timeouts
      - read_body streams bytes; write_body sends bytes
      - info() provides connected/errored and bytes-available if you can support it
- [x] Implement TCP backend on ESP32:
      - lwIP sockets
      - ring/stream buffer for RX, with backpressure
      - write_body sends immediately (or buffered) with sequential offsets
- [x] Decide how to support “bytes waiting”:
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
