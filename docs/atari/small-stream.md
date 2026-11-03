# Atari small stream, revision 1

This optional `$7F` SIO projection is for small Atari clients. Existing Q/W/P/R
and all existing service protocols remain unchanged. This is an opt-in, hardware-validated path: use
firmware and `fn_stream.h` with version 1 support. Older released archives may
not contain this API. The normal packet route remains supported.

The Atari uses a polled 19200-baud POKEY carrier, never SIOV. It does not change
FujiNet's advertised HSIO index, clear U1MB tables, or suppress VBI/DLI. The
firmware must fall back from a previous high-speed disk transfer. The client
retries command frames, then retries an entire stream transaction up to three
times. Hardware timing, especially with arbitrary VBI/DLI work, needs hardware
validation; emulator tests are not that validation.

## Wire

All words are little-endian. Ordinary Atari command/data checksums apply.

| SIO command | AUX | Result |
|---|---|---|
| `s` | 0 | 10-byte capability reply: version=1, maximum=120, token u32, next sequence u32 |
| `S` | request byte length (11..131) | Standard write data phase, ACK then COMPLETE |
| `T` | result receive length (11..131) | Retained result, zero padded to requested length |

`T` shorter than the retained result is NAKed without consuming it. Capability
queries do not reset a connection. The first query lazily allocates a session.
Unsupported firmware NAKs `s`; clients report failure rather than guessing.

Request: `version u8, operation u8, token u32, sequence u32, length u8, data[]`.
Result: `status u8, flags u8, actual u8, token u32, sequence u32, data[]`.
Status uses `StatusCode`: 0 OK, 2 invalid, 3 busy, 4 not ready, 5 I/O error,
6 timeout, 7 internal error, 8 unsupported. Result errors have actual=0.

| Operation | Length/data | Reply length requested by client |
|---|---|---|
| 1 Open | 1..120-byte lowercase `tcp://` URL, no NUL | 11 |
| 2 Read | capacity 1..120, no data | 11 + capacity |
| 3 Write | 1..120 data bytes | 11 |
| 4 Close | zero, no data | 11 |
| 5 Lobby key | key 0..255, no data | 75 |

Open replaces this stream's previous connection and uses NetworkDevice's keep
flag. Successful open means accepted, not necessarily connected; subsequent
operations can return NotReady. DNS/connect behavior is inherited from the
existing backend; this protocol does not introduce another socket stack.

Read flags retain NetworkDevice's EOF, truncated, more-available and
available-valid bits. A short read is normal. A short write advances the cursor
only by actual; the caller submits the remaining suffix as a new operation.
Zero-byte write/half-close, HTTP options, TLS and multiple simultaneous streams
are intentionally outside this experiment.

The cold-path lobby lookup maps creator=1/app=1/key to the existing FileDevice
`persist:///FujiNet/000101xx.key` read (maximum 64 bytes). This prevents pulling
an entire general packet client into the small game just for its title screen.

## Recovery and ownership

A session retains one exact request and its result. Repeating token, sequence
and every request byte replays that result without a second socket operation.
Changing a request at the same sequence is rejected. Advancing by one sequence
implicitly acknowledges the previous result; older/future sequences are
rejected. This also applies to errors and NotReady: retry the identical sequence
for a lost response, but use the next sequence after receiving NotReady to check
again. `T` itself is repeatable. There is no time-based replay expiry.

The Atari validates echoed token/sequence and lengths before advancing its
sequence. Request and result buffers are distinct so a damaged response does
not destroy retry data. Sequence numbers are 32-bit and must not wrap. At
exhaustion the client stops; host reset is required for a new session.

Tokens are issued per ingress session; a host reset releases its connections
and invalidates that token. They are not durable across a firmware restart and
are not authentication. No exactly-once guarantee extends across firmware
restart, application abandonment, or TCP reconnection. After such events,
initialize and open again. There is one foreground owner per ingress; two apps
must not interleave operations on this single-stream interface.

`stream::Session` translates into `IRequestHandler` requests for the existing
NetworkDevice/FileDevice. It never accesses sockets or platform UARTs. Physical
SIO and NetSIO share the same frontend and sequence/replay semantics. Existing
request clients do not allocate the stream buffers unless they issue `s`.

The replay arrays reserve 131 request bytes and 131 result bytes per active
stream, plus metadata, and the SIO projection retains a separate result vector
up to 131 bytes. Routed request/response vectors add bounded temporary storage;
this is not an exact peak-heap cap. No additional task/stack is introduced.

## Tools and validation

Python `fujinet_tools.streamproto` builds/parses payloads with independent wire
vectors and doctests. `scripts/fujinet stream-wire request|response|query` exposes
the codec for hex captures; it does **not** send this SIO protocol over a serial
FujiBus connection. Do not feed these payloads to the generic FujiBus CLI.

`tests/test_stream_session.cpp` exercises routed offsets, partial writes,
NotReady replay, malformed input, stale tokens, SIO checksum refusal, lost
results and interrupted data phases. `test_stream_replay_slot.cpp` covers
bounded replay and sequence exhaustion. Client soft65c02 tests are in the library.
Workspace `run_stream_test.py` boots a diagnostic ATR through real firmware in
headless AltirraSDL and validates fragmented TCP echoes with VBI/DLI/sound.

Current receive policy intentionally uses bounded padding. A one-byte read with
capacity 120 still receives 131 bytes; choose capacity for the application. The
first experiment reduces packet/client complexity and removes the length poll;
it does not claim optimal serial bandwidth for every workload.

## Deployment and architectural trade-off

This projection serves memory-constrained Atari TCP games and interactive clients,
not general service consumers. It removes generic packet construction, cursor
tracking and polling from their hot path. Firmware still routes operations through
NetworkDevice/FileDevice; the SIO layer does not own sockets. Other platforms and
their existing service commands are unaffected. There are no platform conditionals
added to the shared network/file services for this projection.

The application pays for a 262-byte context, partial-transfer/framing logic and
cooperative use of POKEY/interrupts; network calls use polled 19200 rather than
HSIO. Firmware pays the per-active-ingress replay/result storage described above,
plus existing TCP buffers. Receive padding and two SIO exchanges per operation
remain bandwidth costs. This is a code/RAM optimization, not a throughput promise.

A capability query is non-destructive. Close releases the connection but retains
session sequencing/replay; only ingress teardown resets that lifetime. Version 1
must preserve this behavior. An incompatible extension needs version negotiation,
not a silent reinterpretation of existing commands. Standard Q/W/P/R clients
continue to work without opting into these commands.

Hardware acceptance: 459 diagnostic echoes at stock speed and 600 starting with
U1MB HSIO enabled, with DLI/VBI sound and no errors; SYN-ACK WIRE gameplay, disk
help, sound, colours and logoff/rejoin also passed user testing. An observed stale
game/missing-grid issue is unresolved and is not attributed to the transport.
The library's `docs/atari-small-stream.md` owns the application integration guide,
including retry, timeout, register ownership and memory costs.
