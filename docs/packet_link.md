# Packet link over a byte stream

A packet link carries complete raw FujiBus packets over a byte stream (a UART,
a TCP socket or a serial port) for `TransportKind::FujiBusNative`. It is a
proposed stream profile of the [bridge packet ABI](../bridges/rp2350-zorro/docs/bridge-packet-abi.md):
the same one-outstanding exchange, explicit completions, generations and
no-replay rules, over wires that have no packet boundaries of their own.

**Status:** link version 1, implemented by NIO and the Python tools. The bridge
ABI is still a review draft; this profile proposes concrete answers to two of
its open decisions, capacity (C1, reported by the peer in SyncAck) and the
fresh-generation handshake (C2, Sync/SyncAck), and may change with them.

It exists for boards where a microcontroller sits between a host bus and the
ESP32 and is joined to it by a UART, such as the Mac floppy-port board: an
RP2040 serves the Mac's floppy and HD20 signalling and turns what the Mac asks
for into ordinary DiskDevice and other FujiBus requests. NIO sees only FujiBus
packets; it knows nothing about the bus behind the controller.

Related documents:

- [Native packet contract](native-packet-contract.md): `IPacketIO`, `NativeFramer` and their outcomes.
- [Bridge packet ABI](../bridges/rp2350-zorro/docs/bridge-packet-abi.md): exchange, completion and generation rules.
- [Build profiles](build_profiles.md): selecting `FujiBusNative` over a stream channel.

## Roles

- The **controller** (the bridge microcontroller, an emulator, or a test tool)
  starts every exchange and owns generations and retries.
- The **peer** is NIO. It answers; it never sends anything unasked.

## Records

Every record, in both directions:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | sync `F5 4E` |
| 2 | 1 | kind |
| 3 | 1 | generation |
| 4 | 2 | body length, little-endian |
| 6 | n | body |
| 6+n | 4 | CRC-32 (IEEE 802.3, as zlib), little-endian, over offsets 2 to 5+n |

| Kind | Direction | Body |
| --- | --- | --- |
| `0x01` Packet | both | one raw FujiBus packet (`serializeRaw()`) |
| `0x02` Error | peer to controller | one error code |
| `0x10` Sync | controller to peer | `version` (u8): the highest link version the controller speaks |
| `0x11` SyncAck | peer to controller | `version` (u8): the version both now use; `capacity` (u16, little-endian): the peer's raw-packet capacity; `record_timeout_ms` (u16, little-endian): the peer's record timeout |

Later versions may add fields at the end of a Sync or SyncAck body; a receiver
ignores bytes it does not know, and ignores records of a kind it does not know.

| Code | Error | Meaning | Ran? |
| --- | --- | --- | --- |
| `1` | Corrupt | a record whose CRC failed | no |
| `2` | Oversized | a request body over the peer's capacity | no |
| `3` | NotSynchronised | no Sync yet, or a different generation | no |
| `4` | Empty | an empty request body | no |
| `5` | Unanswered | NIO took the request but gave no answer (below) | maybe |
| `6` | UnsupportedVersion | a Sync without a version the peer speaks | (Sync) |

A receiver scans for the sync bytes and ignores anything else. A record whose
length is over capacity, or whose CRC fails, is dropped and scanning resumes
just after its sync bytes. The peer reports it (Oversized or Corrupt) only if it
claims to be a Packet in the current generation while nothing is outstanding;
otherwise it is dropped silently and the controller's timeout covers it. Sync
bytes may appear inside a body; the length and CRC, not the absence of sync
bytes, delimit a record.

## Record timeout

A record is abandoned when its bytes stop for the peer's record timeout. The
limit is on the gap between bytes, not on the whole record, so a large record
on a slow UART still arrives. The peer counts only time it spent waiting for
input as a gap: if NIO is busy elsewhere, the rest of a record waits in the
stream's buffer and is still whole when NIO reads it.

| Stream | Automatic timeout | Why |
| --- | --- | --- |
| UART, serial port | 100 ms | a microcontroller sends a record without pausing |
| TCP | 1000 ms | a lost segment waits at least 200 ms to be resent, longer over a slow network |

`channel.packet_link.record_timeout_ms` overrides it (1 to 60000 ms; 0 is
automatic), and so does `link.set` on the console. The peer advertises the
value it uses in every SyncAck.

Because a record cut off part way (a controller restarting mid-record, say)
would otherwise take its length from whatever follows, a controller stays
quiet before every Sync for a little longer than the peer's record timeout:
the value from the last SyncAck, or 100 ms before the first. If a Sync gets no
SyncAck, the controller doubles the pause for the next attempt (up to a few
seconds), which reaches any peer's setting without knowing it in advance.

## Capacity

Each peer has a fixed raw-packet capacity, 4096 bytes by default
(`channel.packet_link.capacity`, 64 to 65535; it takes effect when NIO
starts), reported in SyncAck. It rejects larger request bodies with Oversized. A controller must
keep requests within it and must not ask for answers larger than it, splitting
larger work: for example reading a floppy track with more than one
`ReadSectors`, or a file with several reads no larger than the capacity.

## Generations

1. After a quiet pause, the controller sends Sync with a new generation `g`, any time.
2. The peer completes an outstanding request with Unanswered in its old
   generation, discards every queued and partial packet, adopts `g` and
   answers SyncAck `g` with the agreed version, its capacity and its record
   timeout. If the Sync
   has no version the peer speaks, it answers Error UnsupportedVersion in `g`
   and stays unsynchronised.
3. Only then may the controller send requests, each as a Packet in `g`.

The controller starts a new generation at startup, after a timeout, and after
any completion it cannot match to its request. A peer restart loses the
generation, so its next answer is NotSynchronised and the controller resyncs.

## Exchanges

One request is outstanding at a time:

1. the controller sends a Packet in the current generation;
2. the peer answers with exactly one completion in that generation: the
   response Packet, or an Error;
3. the controller takes that completion before sending another request.

An Error other than Unanswered means the request was not executed; the
controller may send it again as a new request. Unanswered and a timeout are
unknown completions: the request may or may not have run, so the controller
starts a new generation and does not resend it. Whether to retry at a higher
level, for example returning a disk error to the host, is the controller's
decision.

NIO answers each request before it reads the next one: `IOService` handles a
request and sends its response in the same pass. The link relies on that. A
request still outstanding when NIO next reads from the link got no answer, so
the link completes it with Unanswered at once rather than leaving the controller
to time out. That happens when the body is not a valid FujiBus packet, or when
its answer cannot be sent (larger than the capacity, or than a raw FujiBus
packet can be). The request may have run in the second case, which is why
Unanswered is an unknown completion. The same rule means a late answer can
never complete a request from a later generation. If NIO ever handles requests
asynchronously, the link needs a different way to tie answers to requests.

## NIO

`io::PacketLink` implements `IPacketIO` on top of any byte `Channel`, and
`io::PacketLinkChannel` owns both and exposes the link as `packet_io()`. A
build profile asks for a link with `packetLink = true` (with
`TransportKind::FujiBusNative`); the POSIX and ESP32 channel factories then wrap
its stream channel (TCP or serial on POSIX, UART or USB CDC on ESP32) through
`io::with_packet_link`. Profiles that don't ask, such as the Zorro placeholder
over a Pty, are unchanged.

The POSIX presets are `fujibus-link-tcp-debug` (a TCP server) and
`fujibus-link-serial-debug` (a serial port, such as the USB CDC device of a
bridge board, set by `channel.serial_port` and `channel.uart` in `fujinet.yaml`
or by `FN_SERIAL_PORT` and `FN_SERIAL_BAUD`; see [build profiles](build_profiles.md)).

### Configuration

```yaml
channel:
  packet_link:
    capacity: 0            # bytes; 0 = 4096
    record_timeout_ms: 0   # 0 = automatic (100 ms UART/serial, 1000 ms TCP)
```

### Console

The `link` diagnostics provider is registered when the channel is a packet
link (see [diagnostics](diagnostics.md)):

- `link.status`: state, generation, agreed version, capacity, record timeout,
  the stored settings, and counters (syncs, requests, answers, unanswered,
  refused, corrupt, oversized, abandoned);
- `link.set record_timeout_ms <ms>`: takes effect at once; controllers learn it
  at their next Sync;
- `link.set capacity <bytes>`: stored, applied when NIO next starts;
- `link.save`: writes `channel.packet_link` to `fujinet.yaml`.

On ESP32 the `uart.*` commands still reach the UART underneath the link.
