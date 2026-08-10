# FujiNet Driver and Channel Architecture

This document defines the client-side architecture for operating-system disk
drivers and other FujiNet clients. It complements
[`disk_device_protocol.md`](disk_device_protocol.md), which defines the NIO
server-side block-device contract.

The Amiga floppy-port/Pico channel design is described separately in
[`amiga-floppy-channel.md`](amiga-floppy-channel.md).

The immediate target is an Amiga `fujinet-disk.device` client, with the
existing MS-DOS driver as the first related implementation. The design is
intended to support RS-232 initially and faster channels such as Zorro later,
without creating a separate disk-driver implementation for every channel.

## The layers

```text
OS disk driver
        |
        v
DiskDevice client codec
        |
        v
FujiBus packet protocol
        |
        v
Channel framing / transport adapter
        |
        v
RS-232, TCP, Zorro, USB, or another physical/logical channel
```

### OS driver

The OS driver exposes native operating-system semantics. For Amiga this is
the proposed `fujinet-disk.device`, which should present a block-device
interface compatible with AmigaDOS and MountList configuration. Amiga
applications should use normal AmigaDOS operations such as `Open`, `Read`,
`Write`, `Dir`, and `Type`; they should not construct FujiBus or DiskDevice
requests directly.

The driver owns:

- device and unit lifecycle;
- unit-to-DiskDevice-slot mapping;
- request queueing and serialization;
- native OS error translation;
- geometry and media state visible to the OS;
- caching and write policy;
- media-change and reinitialization behavior;
- MountList/CLI integration appropriate to the target OS.

NIO does not own these responsibilities and must not parse AmigaDOS, OFS,
FFS, directory entries, or MountLists.

### DiskDevice client codec

This is a typed client API for the existing generic DiskDevice protocol. It
should be reusable by the Amiga driver, MS-DOS driver, test tools, and future
clients. It belongs in `fujinet-nio-lib` or a small shared driver-protocol
library, not in each application.

The API should provide typed operations equivalent to:

```text
disk_mount(slot, uri, mode, type, sector_size_hint)
disk_unmount(slot)
disk_info(slot)
disk_read_sector(slot, lba, buffer, capacity)
disk_write_sector(slot, lba, buffer, length)
disk_clear_changed(slot)
```

The codec owns little-endian encoding, version fields, payload lengths,
response validation, DiskDevice status values, and geometry parsing. It does
not own the channel or OS driver policy.

The existing low-level `fn_raw_call()` remains useful as a primitive for
control applications, but applications should not duplicate DiskDevice
payload layouts when a typed disk API exists.

### FujiBus packet protocol

FujiBus defines the logical request and response exchanged with NIO:

- device ID, including DiskDevice `0xFC`;
- command ID;
- request/response payload;
- status and length fields;
- packet checksum and protocol versioning.

FujiBus is independent of the physical channel. The DiskDevice command set
does not change when the channel changes.

### Channel framing and transport

SLIP is the existing byte-stream framing mechanism used by the current
RS-232, PTY, and TCP-style FujiBus paths. For example:

```text
FujiBus packet -> SLIP encode -> serial.device -> RS-232
```

“FujiBus” and “SLIP” should not be used as interchangeable names. FujiBus is
the logical packet protocol; SLIP is one framing layer used to carry those
packets.

A channel adapter owns:

- opening and closing the physical/logical channel;
- sending and receiving bytes or native packets;
- channel-specific timeouts and flow control;
- channel capabilities and maximum transfer sizes;
- translating the channel into the interface expected by the FujiBus client.

For RS-232 and existing TCP/PTY compatibility paths, SLIP remains appropriate.
For new high-speed channels—SPI, the Amiga floppy-port/Pico link, Zorro, or a
parallel interface—the preferred design is packet-native FujiBus transport,
without SLIP. Such a channel should provide packet boundaries, length, and
integrity checking in its own link envelope while preserving the FujiBus packet
and DiskDevice commands unchanged.

For the Amiga work, these typed DiskDevice codecs belong in
`fujinet-nio-lib`. The driver repository may provide thin AmigaOS adapters,
but it should not fork the wire-payload implementation. This keeps the
codec available to `fnctl`-style tools and other clients as well as the Amiga
driver.

This is a deliberate direction rather than an unresolved choice: SLIP is for
legacy or stream compatibility; new high-speed interfaces should not add SLIP
overhead unless a concrete interoperability requirement justifies it.

## Channel independence of the Amiga driver

The Amiga disk driver should depend on an abstract channel/session interface,
not directly on `serial.device`, TCP sockets, or Zorro registers. A minimal
interface is conceptually:

```text
channel_open(configuration)
channel_close()
channel_send_packet(packet)
channel_receive_packet(packet, timeout)
channel_flush()
channel_capabilities()
```

The first session contract is deliberately small and request/response based:

```text
open(configuration) -> Result<Capabilities>
close()
request(fujibus_packet, timeout) -> Result<fujibus_packet>
flush() -> Result<void>
capabilities() -> Capabilities
```

`request` sends one complete FujiBus packet and waits for its matching
response. The initial Amiga driver permits one outstanding request per
session; channel implementations must preserve request/response ordering.
The returned capabilities report at least maximum packet size, maximum
payload size, whether the channel is byte-stream/SLIP or packet-native, and
the maximum number of outstanding requests (which is `1` initially).

Timeout, framing/checksum failure, peer reset, and transport I/O failure are
distinct session errors. A failed request does not get retried implicitly by
the channel: retry policy belongs to the driver/session owner, which may
flush, reset, reopen, and retry only idempotent operations. The exact
packet-envelope fields for packet-native links remain channel-specific, but
they must deliver the same FujiBus packet contract above.

The actual implementation may use byte-oriented operations for a legacy SLIP
stream, or packet-oriented operations for a new high-speed link. The important
rule is that the driver above this interface sees FujiBus packets, not physical
bytes or SLIP details.

There may be separate channel binaries or build variants because different
channels must know how to access their hardware. There should not be separate
copies of the Amiga disk-driver logic. The intended structure is:

```text
same fujinet-disk.device logic
        |
        +-- RS-232 channel backend
        +-- TCP channel backend
        +-- Zorro channel backend
```

The first implementation can select a backend at build time. Runtime channel
selection can be added later if the Amiga boot/configuration model benefits
from it.

## Persistent driver versus short-lived applications

The current Amiga library transport is suitable for applications that issue a
request and exit. A disk driver has different requirements:

- one persistent channel session;
- serialized or safely multiplexed outstanding requests;
- no process-exit cleanup assumptions;
- bounded request queues;
- recovery after timeout or link loss;
- predictable behavior for 512-byte block transfers;
- channel-specific performance reporting.

The driver may reuse common FujiBus framing and codecs, but it should not
simply call an application-oriented global transport without adapting its
lifecycle and concurrency model.

## Proposed repository layout

The existing `fujinet-nio-driver` repository is the home for the broader
driver repository. Its MS-DOS implementation will move under a platform
directory. Existing workspace build variables retain a
The workspace build uses `FUJINET_NIO_DRIVER` for this repository:

```text
fujinet-nio-driver/
  README.md
  contracts/
  common/
    fujibus/
    disk_protocol/
    channel/
  msdos/
    ...
  amiga/
    disk.device/
    channels/
      rs232/
      tcp/
      zorro/
```

Shared directories should contain protocol and interfaces, not OS-specific
disk semantics. MS-DOS and Amiga should share codecs, status mappings, and
channel concepts where useful, while retaining independent native drivers.

The repository move should keep the root build entry point and generated
artifact path at their documented locations so workspace build scripts remain
simple and deterministic.

## Amiga implementation stages

### Stage 1: protocol client

- Add typed DiskDevice request/response codecs to `fujinet-nio-lib`.
- Validate exact geometry and payload lengths.
- Add tests using captured FujiBus payloads.
- Keep the API independent of RS-232 and AmigaOS.

### Stage 2: minimal Amiga driver

- Implement one unit mapped to DiskDevice slot 1.
- Mount an ADF and obtain geometry through `Mount`/`Info`.
- Implement read-only block reads using complete 512-byte sectors.
- Translate NIO status codes to Amiga device errors.
- Use the session abstraction, with RS-232 as the first backend.

### Stage 3: Amiga integration

- Add MountList/CLI configuration.
- Boot a driver-containing ADF in Amiberry.
- Mount a configured ADF.
- Exercise `Dir`, `Type`, and known-file reads. Keep the mounted unit
  read-only in this phase.
- Capture CLI output/framebuffer and retain NIO traffic logs.

### Stage 4: robustness and performance

- Media-change and cache invalidation behavior.
- Timeout/link-loss recovery.
- Multiple units and runtime mount recovery.
- Larger request batching using `ReadSectors` where useful; writes remain a
  later milestone with an explicit flush and failure policy.
- Zorro channel backend and throughput measurements.

RS-232 should remain a valid correctness and hardware test path, but it should
not constrain the driver API or the NIO disk service. Full-disk use will likely
require a faster channel.

## Decisions to record for each new channel

Every new channel should document:

- whether it carries raw bytes or complete FujiBus packets;
- whether the channel is a legacy SLIP byte stream or a preferred packet-native
  FujiBus link;
- maximum request and response sizes;
- ordering and delivery guarantees;
- timeout and reset behavior;
- whether multiple outstanding requests are supported;
- how the Amiga driver selects and configures it;
- measured read/write throughput and latency.

The channel must not introduce Amiga-specific commands into DiskDevice. If a
channel needs setup or discovery commands, those belong to a channel-control
layer with its own versioning.

## Non-goals

This architecture does not propose:

- an Amiga-specific NIO wire device;
- AmigaDOS parsing in NIO;
- a separate DiskDevice command set per channel;
- separate copies of the Amiga disk driver for RS-232 and Zorro;
- making application utilities responsible for block-device protocol details.
