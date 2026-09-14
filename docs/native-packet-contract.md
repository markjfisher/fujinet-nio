# Native FujiBus packet contract

This is the software packet adapter contract established by Stories 1.2–1.4.
It supports deterministic host testing before a physical backend exists.
The Story 1.5 follow-up adds software backend containment through both actual
retry callers. See the
[workspace evidence record](../../../_bmad-output/specs/spec-amiga-zorro-ii-packet-native-backend/stories/1-6-accept-the-canonical-software-packet-contract-for-bridge-design.md#technical-acceptance-record--2026-09-15)
(available in the parent workspace layout) for Story 1.6's decision, exact
revisions, checks and limits. This document does not
approve a Zorro register layout, bridge link, physical timing or hardware readiness.

## Raw representation

`FujiBusPacket::serializeRaw()` and `fromRaw()` use the existing six-byte header:
device at offset 0, command at 1, little-endian total length at 2–3, folded
checksum at 4, first parameter descriptor at 5. Additional descriptors precede
parameters; the remaining bytes are opaque payload. The checksum treats its own
byte as zero. Parsing requires the supplied byte count to equal the declared
length and rejects corrupt checksums and truncated descriptors/parameters.
See [the protocol reference](protocol_reference.md) for exact field rules and
legacy compatibility behavior.

Raw packets carry no SLIP delimiters or escaping. `0xC0` and `0xDB` are ordinary
bytes, including in headers. `IFramer` exchanges raw packets with
`FujiBusTransport`; `SlipFramer` and the legacy serial codec wrappers share SLIP
helpers. Response status remains U8 parameter 0. No command, service payload,
correlation field or response mapping is added. This describes canonical encoder
output and C++ transport mapping; it does not assert identical permissive parser
behavior in every language. Canonical responses with one U8 status parameter
are supported. The C response parser's pre-existing permissive malformed-input
differences are informational, not a universal parser-equivalence requirement
or extra acceptance gate. Fixtures belong in the retry follow-up only where
they affect actual `fn_raw_call` fault behavior; no parser change is implied.

The canonical raw length is 6–65,535 bytes. A packet adapter can impose a smaller
capacity. SLIP expansion (up to twice the raw size plus two delimiters), raw
length and a physical transfer capacity are different quantities. The software
ceiling is not a proposed bridge buffer size or a guarantee that every platform
can allocate that much memory.

## Why a packet capability is needed

`Channel::read()` reports a byte count without record boundaries, and `write()`
has no result. Neither a short read nor a poll interval proves that a packet
ended. Native framing therefore uses an explicit packet capability exposed by
`Channel`, with no byte-stream fallback. Existing serial channels keep their
byte interface. Native bootstrap rejects a channel without usable packet
capability; the PTY-backed Zorro placeholder is not a functional native backend.

The adapter supplies a stable capacity, a bounded nonblocking whole-packet
receive operation, a single-attempt send operation with an observable result,
and a local reset operation with an observable result. The capability belongs
to its channel and must outlive its framer. The channel must remain alive while
the framer holds a ready packet, because extraction rechecks its capability.
A framer stays bound to one capability;
substituting another cannot release packets retained from its predecessor.
NativeFramer cannot be copied or moved, so its ready slot has one owner.
One active framer must exclusively drive an adapter. Calls are serialized;
interleaving independent framers or resetting an adapter behind a framer with
an outstanding slot is unsupported. Retire the old framer and discard its slot
before handing off the adapter. A handoff does not clear adapter uncertainty.

The interface is `IPacketIO` in `include/fujinet/io/core/packet_io.h`:

```cpp
capacity() const;                       // immutable byte capacity
receive(buffer, capacity);              // PacketReceiveResult { status, size }
send(buffer, size);                     // PacketIOStatus
reset();                                // PacketIOStatus, local state only
```

`Channel::packet_io()` returns the capability or null. `NativeFramer` exposes
`receiveStatus()`, `sendStatus()`, `resetStatus()`, `unknownCompletion()` and
`reset(Channel&)`. Status accessors describe their respective operation, so a
receive poll does not overwrite send evidence.
These are last-operation results, not readiness flags. Use `nextPacket()` for
delivery; a previous `Ok` does not make data available after extraction or reset.

## Ownership and bounded work

The adapter owns queued records and incomplete assembly. Each implementation
must bound both record count and bytes. Caller buffers are borrowed only for the
operation; the adapter never retains their pointers. A successful receive copies
one complete nonempty record into the provided capacity. It never returns a
prefix as a valid packet. A successful send accepts an owned copy locally; the
caller can release or change its input immediately afterward.

NativeFramer owns at most one ready packet. It performs at most one adapter
receive per poll and leaves the adapter untouched while that slot is occupied.
Repeated polls neither merge queued packets nor overwrite the ready packet.
Extraction transfers the complete record once; failed extraction clears the
caller's output. Effective storage is bounded by the adapter's capacity and the
raw size ceiling. Zero capacity is unusable.

Partial delivery remains private to the adapter. Incomplete input is not exposed
until explicit completion. Empty, oversize and truncated records have explicit
failure outcomes and are discarded without leaving a prefix for the next record.
The framer carries opaque bytes: FujiBus field/checksum validation remains in
the codec/transport, not in packet I/O.

## Transfer outcomes

Receive, send and reset outcomes are separately observable. Polling cannot erase
a recorded send failure. No-data/incomplete input and queue backpressure are
separate from malformed records, unavailable peers, definite transfer failure
and unknown completion.

| Result | Meaning |
| --- | --- |
| `Ok` | Complete receive, local send acceptance, or successful local reset |
| `NoData`, `Incomplete` | No complete receive record yet (`NoData` also initializes unattempted operations) |
| `EmptyPacket`, `Oversized`, `Truncated` | Rejected record/input; no valid prefix delivered |
| `Backpressure` | Capacity is occupied; no additional send accepted |
| `Unavailable`, `SendFailed` | Peer unavailable or definite send rejection |
| `UnknownCompletion` | Acceptance/effect cannot be established; uncertainty remains latched |
| `ResetFailed`, `ResetRequired` | Reset failed or ordinary operation remains locked |
| `Unsupported`, `InvalidCapacity`, `AdapterChanged` | Missing capability, unusable capacity, or identity mismatch |

Adapter implementations return only the outcomes allowed for their operation:

| Adapter operation | Allowed results |
| --- | --- |
| Receive | `Ok`, `NoData`, `Incomplete`, `Truncated`, `EmptyPacket`, `Oversized`, `Unavailable`, `ResetRequired`, `UnknownCompletion` |
| Send | `Ok`, `Backpressure`, `EmptyPacket`, `Oversized`, `Unavailable`, `SendFailed`, `ResetRequired`, `UnknownCompletion` |
| Local reset | `Ok`, `ResetFailed`, `Unavailable`, `UnknownCompletion` |

`Unsupported`, `InvalidCapacity` and `AdapterChanged` are framer capability-check
results. Every non-`Ok` reset result keeps ordinary I/O locked. `ResetRequired`
from receive or send also locks the framer and invalidates its ready slot.
`UnknownCompletion` from any operation latches uncertainty, including from reset;
a subsequent successful local reset cannot erase it.

A full receive slot stops reads. A full transmit queue rejects the send without
accepting any part of it. Each send call makes at most one adapter attempt; the
framer neither queues hidden retries nor repeats a failed send. A definite
rejection guarantees no local acceptance. Local acceptance does not prove
remote execution, a remote effect, or response delivery.

Receive reports a nonzero byte count only with `Ok`; other receive outcomes
report zero. `NoData` and `Incomplete` are receive outcomes, not successful sends.
Definite send rejection does not itself require a reset: another explicit send
may succeed when the adapter is available. The framer does not make that attempt
on the caller's behalf.

`FujiBusTransport::send()` also forwards an empty serialization result to its
framer. Native framing reports `EmptyPacket` without attempting adapter I/O, so
a response exceeding the raw codec limit cannot leave an earlier send success
as its apparent outcome. A nonempty raw packet exceeding the adapter's smaller
capacity reports `Oversized`. Serial framing still drops empty input.

Unknown completion is retained as an uncertainty condition and blocks further
use even across a successful local reset. NativeFramer has no recovery API
that invents remote quiescence. The separate Amiga backend guard described
below permits explicit recovery only with independently established quiescence.
Framer result inspection is the observable software
seam; the service-facing `ITransport` interface remains unchanged.

## Reset and stale state

Every reset attempt discards the framer's ready packet. Successful adapter reset
discards queued, incomplete and scheduled local data, including local transmit
bookkeeping. Reset failure may leave adapter state pending and leaves the native
path locked until reset succeeds. Successful
local reset permits ordinary local operation only when completion was not
ambiguous; it cannot clear the unknown-completion condition.

Discarding a local buffer does not undo an already transmitted write. Closing,
reopening, waiting or clearing a local queue does not prove the remote endpoint
has stopped. A late packet produced by a peer after reset requires a separately
proven ownership/recovery protocol. The deterministic double's ability to clear
its own scheduled events is evidence about local state, not about physical peer
reset or stale-response isolation across a real link.

## Verification and remaining gates

`tests/test_native_framer.cpp` exercises production framing with the bounded
opaque double in `tests/packet_io_double.h`: queued/delayed packets, repeated
polls, partial completion and truncation, empty/exact/oversize records,
backpressure, send failures, unknown completion, reset and unsupported channels.
The double transfers bytes and schedules faults; it implements no FujiBus codec
or service. Transport composition uses independent literal wire fixtures.
Serial and Atari SIO regressions run in the same host suite.

From the workspace, source `scripts/env.sh`, then in this repository run:

```sh
./build.sh -cp fujibus-pty-debug
ctest --test-dir build/fujibus-pty-debug -R '^fujinet-nio-tests$' --output-on-failure
```

## Amiga backend containment and caller evidence

The driver provides `amiga/nio.device/fujinet_nio_packet_backend.[ch]`, a
portable whole-raw-packet guard behind the broker backend seam. It is linked
with the actual Amiga transport, broker, disk read/write client and `fn_raw_call`
in `amiga/tests/test_fujinet_nio_packet_backend.c`. The deployed serial backend
is unchanged; this component does not select or implement a physical adapter.

The guard starts quarantined and borrows exclusive scratch storage of
6–65,535 bytes for its lifetime, disjoint from exchange request/response buffers.
Its serialized callback interface distinguishes definite
rejection (nothing sent), completed exchange (no further effect or reply from
that exchange), and unknown completion. Local send acceptance is insufficient.
It validates raw structure, length, checksum and matching device/command before
copying a completed reply. Device/command matching is not correlation. Invalid
or missing replies leave quarantine set and report zero failed response length.
No exchange request/response pointer is retained; the adapter must obey the
supplied capacity. The guard retains only its dedicated scratch pointer.

Unknown completion survives close/open. Every local reset attempt quarantines,
even from a healthy state. Only explicit recovery whose callback proves that
prior work can neither execute nor deliver an old reply clears quarantine.
The callback may fail; local reset, reopening, delay or clearing a buffer is
never such proof. Definite rejection or valid known completion can clear the
temporary quarantine set before a transfer; neither recovers a previously
quarantined endpoint. Calls are serialized by the owning worker; the reentry flag
is not a concurrency lock. See the driver
[adapter obligations](../../fujinet-nio-driver/amiga/README.md#undeployed-whole-packet-containment-component)
for implementation and lifecycle requirements.

The independent peer deliberately accepts unsafe sends and retains pending
work/late responses across local lifecycle operations. Tests count attempts,
backend entries, transmissions, effects, pending work and replies separately.
Both callers execute pre-send rejection/open failure, post-delivery ambiguity,
post-effect lost/corrupt/oversized/truncated replies, lifecycle/reset failures,
late same-command replies, and failed/successful quiescence. Queue/active abort
and buffer ownership tests supplement the six core fault rows. Max remote
in-flight count is one; quarantine blocks additional transmissions until proof.
The corrected existing retry test also verifies the second call's actual
script indices, diagnostics and independent buffers.

This is ambiguity containment, not universal exactly-once execution. Existing
`fn_raw_call` policy can replay after known completion if a valid reply exceeds
the application's reply capacity, or after a completed active abort. Tests
characterize those cases explicitly. The backend cannot see that application
capacity; retry/service semantics remain unchanged. The guard preserves
canonical U8 status bytes; `fn_raw_call` exposes them unchanged, while existing
service-specific mappings remain intact. Pre-existing permissive
parser differences remain informational, with no universal parser-equivalence
gate added by this work.

Driver verification (source the workspace environment first):

```sh
cd repos/fujinet-nio-driver/amiga/tests
make test
```

This includes the integration target, broker and corrected retry tests. The
component also cross-compiles for 68000. Story 1.6 records exact commands,
results, independent review and full owner revisions. The reviewed driver
implementation is `e6f9686797f6bae256342d362795c4b3fc5b3da1`, linking unchanged
library revision `dac8bf66c4ec44841790e08021c1654379c21255`. The firmware gate above
passes 344 cases / 6,856 assertions and the registered Python suite.

Story 2.4 consumes the accepted 1.6 decision plus positive relevant 2.2/2.3
physical feasibility and explicit human ABI approval. Pin both the firmware
contract commit and the workspace commit containing the reviewed acceptance;
never silently substitute a newer version. Changes require renewed acceptance
and downstream ABI impact review. Physical implementation also requires 1.14.
The concrete physical quiescence mechanism, real-service parity and guest
integration remain downstream work. Serial remains a separate compatibility
deployment, with no native fallback or automatic physical failover.
