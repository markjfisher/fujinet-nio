# Response to Jeff's Amiga Disk Device Plan

**Draft for discussion**  
**Source plan:** [plan-amiga-disk-device.md](https://github.com/jeffpiep/amiga-fujinet/blob/dev/docs/plan-amiga-disk-device.md)

## Short version

We are strongly aligned on the core shape of this work:

- `fujinet-disk.device` should present a normal Amiga block device.
- AmigaDOS should own OFS/FFS, directories, files, boot blocks, and MountList
  behavior.
- NIO should provide numbered fixed-size blocks and geometry, not an Amiga
  filesystem implementation.
- The Amiga device API should remain stable while the underlying transport
  changes from RS-232 to a faster PHY such as Zorro or a parallel interface.

The main points we need to make explicit are:

1. NIO has now implemented the generic ADF/block-device side using the
   existing DiskDevice protocol at wire device `0xFC`.
2. RS-232 should remain available as a correctness and hardware test path,
   even though it is not suitable for practical full-disk use at 19200 baud.
3. The Amiga driver should start against an abstract FujiNet channel, not
   directly against `serial.device` or a particular PHY.
4. The driver source should be developed in a broader driver repository, while
   reusable FujiBus and DiskDevice codecs can live in `fujinet-nio-lib` or a
   shared driver library.
5. Hot-swap and larger HDF/RDB semantics are intentionally later phases.

This is intended to prevent duplicate work and to give us a shared boundary
before either side commits deeply to implementation details.

## Point-by-point response

### Goal: a normal AmigaDOS volume

Agreed.

The target should be a normal Amiga device/volume relationship:

```text
AmigaDOS / ROM filesystem
        |
        v
fujinet-disk.device
        |
        v
FujiNet DiskDevice client
        |
        v
FujiBus / channel
```

Applications should be able to use `Dir`, `Type`, `Copy`, and normal file APIs
without knowing that the blocks are remote. They should not construct raw
DiskDevice payloads themselves.

The NIO side will not parse AmigaDOS, OFS, FFS, directory structures,
MountLists, or boot-block metadata. Those remain Amiga-side responsibilities.

### Split at the block boundary

Agreed completely.

NIO now exposes the generic `DiskDevice` block interface. It does not add an
Amiga-specific FujiNet device or Amiga-specific command numbers.

The initial Amiga profile is:

- mount an ADF into DiskDevice slot 1;
- obtain geometry from the Mount response or `Info`;
- read and write 512-byte blocks using `ReadSector` and `WriteSector`;
- use `ClearChanged`, `Unmount`, and runtime-mount recovery where needed;
- let AmigaDOS interpret the resulting media.

The server-side contract is documented in
[`docs/disk_device_protocol.md`](../docs/disk_device_protocol.md).

### What NIO provides

The following work is complete in `fujinet-nio`:

- automatic, case-insensitive `.adf` recognition;
- ADF exposed as generic `ImageType::Raw`;
- 512-byte sector geometry;
- a standard 880 KiB ADF reported as 1760 blocks;
- rejection of non-512-aligned ADF sizes;
- generic raw sector reads and writes;
- mount, unmount, geometry/info, changed-state, and runtime recovery;
- read-only, out-of-range, malformed-image, and short-buffer status handling;
- unit tests for first/final block access and wire payloads;
- a POSIX integration scenario using a deterministic 880 KiB ADF fixture.

The relevant point is that this is generic disk support. There is no
Amiga-specific conditional compilation or Amiga filesystem logic in the NIO
disk service.

NIO will also remain responsible for transport-neutral server behavior. The
same DiskService should serve Atari, BBC, MS-DOS, and Amiga clients.

### What the Amiga driver provides

The Amiga driver still needs to be written. It should own:

- the Exec device implementation;
- `CMD_READ`/`CMD_WRITE` and the required trackdisk subset;
- unit-to-DiskDevice-slot mapping;
- MountList and CLI integration;
- native Amiga error translation;
- request queueing and serialization;
- geometry and media state exposed to AmigaDOS;
- caching and write policy;
- media-change behavior when that is introduced;
- channel/session lifecycle from the Amiga side.

The driver should not duplicate FujiBus packet construction in every command
or application. Applications such as `fnctl` may continue to use low-level
raw calls for administration and diagnostics, but disk I/O should go through
the driver.

## Transport, FujiBus, and SLIP

There is no disagreement about using FujiBus. We should just use the terms
precisely:

- **FujiBus** is the logical request/response packet protocol: device ID,
  command, payload, status, lengths, checksum, and versioning.
- **SLIP** is byte-stream framing used to delimit FujiBus packets on a stream.
- **Channel** is the physical or logical path: RS-232, TCP, Zorro, parallel
  port, USB, or another mechanism.

The RS-232 path is:

```text
DiskDevice request
    -> FujiBus packet
    -> SLIP framing
    -> serial.device
    -> RS-232
```

For TCP/Amiberry, the same FujiBus-over-SLIP byte stream is appropriate.

For Zorro or a shared-memory/mailbox PHY, we have two possible options:

1. Present a byte-stream abstraction and retain SLIP for maximum reuse.
2. Carry the same FujiBus packet inside a native Zorro mailbox frame and omit
   redundant SLIP framing.

Either option is compatible with the NIO DiskDevice contract. A native Zorro
outer frame should not lead to a new Amiga-specific DiskDevice command set.
The choice belongs to the channel contract.

The Amiga disk driver should therefore depend on an abstract channel/session
interface, conceptually:

```text
channel_open(configuration)
channel_close()
channel_send_packet(packet)
channel_receive_packet(packet, timeout)
channel_flush()
channel_capabilities()
```

The first implementation may select the backend at build time. We should not
maintain separate copies of the disk-driver logic for RS-232, TCP, and Zorro.
There can be separate channel backends or binaries because they must know how
to access their hardware.

## RS-232 position

The plan is correct that 19200-baud RS-232 is not practical for full-disk
operation. We should not design the user experience around transferring an
880 KiB disk this way.

However, we do not think RS-232 should be treated as entirely blocked. We have
physical RS-232 FujiNet hardware available, so it is valuable for:

- validating the Amiga Exec device lifecycle;
- validating MountList and AmigaDOS integration;
- checking geometry and media behavior;
- exercising real FujiBus request/response handling;
- testing error recovery and timeout paths;
- providing a regression path while a faster PHY is developed.

The practical split is:

- RS-232: correctness, smoke tests, small reads, and development diagnostics;
- TCP: initial fast Amiberry/emulator validation;
- Zorro/parallel/future PHY: practical disk performance.

The driver API must not encode the RS-232 throughput assumption.

## Geometry decision

For the first ADF profile, we should commit to standard DD geometry:

```text
sector size:  512 bytes
sector count: 1760 blocks
image size:   880 KiB
```

That matches ordinary 880 KiB ADF files and gives us a concrete end-to-end
acceptance target. The generic NIO probe also supports other exact multiples of
512 as raw ADF media, but the first Amiga integration should use 1760 blocks.

We should not automatically interpret `.hdf` as an Amiga hard disk. HDF/RDB
semantics, partition geometry, and larger-volume policy need a separate design.
The driver can later support a larger geometry once the server-side image and
Amiga-side MountList/RDB contract is agreed.

## Trackdisk command scope

The NIO protocol already provides the primitives needed by the first driver:

| Amiga-side need | DiskDevice operation |
|---|---|
| Mount/configure media | `Mount (0x01)` |
| Read one block | `ReadSector (0x03)` |
| Write one block | `WriteSector (0x04)` |
| Report geometry | Mount response / `Info (0x05)` |
| Clear media-change state | `ClearChanged (0x06)` |
| Remove media | `Unmount (0x02)` |

The Amiga driver maps native `CMD_READ`/`CMD_WRITE` requests onto these
operations. We do not need a new NIO command that mirrors the Amiga Exec API.

The first driver should implement the smallest trackdisk subset needed by
AmigaDOS and the test MountList. `TD_CHANGESTATE`, `TD_CHANGENUM`,
`TD_PROTSTATUS`, `CMD_UPDATE`, and related commands should be implemented when
we know which ones the target Kickstart/ROM filesystem actually calls.

## Media change and hot swap

NIO supports the existing changed flag and explicit mount/unmount behavior,
but hot-swapping a backing image under a running Amiga filesystem is not part
of the first phase.

For the initial driver:

- treat a mounted unit as stable for the session;
- use explicit unmount/remount for media replacement;
- expose/clear the existing changed state where useful;
- avoid claiming safe media removal while AmigaDOS may have cached blocks.

When hot swap is added, it must be designed jointly. It requires coordination
between NIO mount state, driver notifications, Amiga cache invalidation, and
the native change-state commands.

## Write support

NIO already supports complete-sector writes and verifies short writes,
read-only media, and out-of-range writes. The wire contract requires a full
512-byte block.

The first Amiga driver may choose either of these delivery strategies:

- implement reads first and keep the device read-only until cache/write policy
  is settled; or
- implement complete-block writes immediately and test write/reread behavior.

Both are compatible with the NIO work. The choice is a driver milestone, not a
server-side blocker. If writes are enabled, the driver must define flush and
failure behavior rather than silently assuming every write is durable.

## Where the code should live

The proposed expansion of `fujinet-nio-msdos` is a good direction. We suggest
renaming it to `fujinet-nio-driver` while preserving history, then moving the
existing MS-DOS implementation under a platform directory:

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

Suggested ownership:

| Area | Proposed home |
|---|---|
| NIO DiskService, ADF probe, image handling | `fujinet-nio` |
| Generic DiskDevice wire contract | `fujinet-nio` documentation + tests |
| Typed client DiskDevice codecs | `fujinet-nio-lib` or driver `common/` |
| FujiBus packet codec | shared library/common driver code |
| Amiga Exec device | `fujinet-nio-driver/amiga` |
| Amiga MountList/startup tooling | `fujinet-nio-driver/amiga` |
| RS-232/Zorro channel backends | `fujinet-nio-driver/amiga/channels` |
| AmigaDOS/OFS/FFS behavior | AmigaOS/driver side, never NIO |

The existing `fn_raw_call()` should remain available for control utilities,
but the driver and applications should use typed helpers for DiskDevice
operations. This avoids every application encoding version bytes, little-endian
fields, geometry, and response lengths independently.

The driver itself is long-lived and should not rely blindly on an
application-oriented global transport with process-exit cleanup. It needs a
persistent session, serialized requests, bounded queues, timeout recovery, and
channel capability reporting.

## Proposed shared milestones

### Milestone 1: agree the contracts

- Confirm this ownership split.
- Confirm `0xFC` and the existing DiskDevice v1 command set as the wire API.
- Confirm 512-byte sectors and 1760 blocks for the first ADF profile.
- Confirm no HDF/RDB inference in the first phase.
- Decide whether typed DiskDevice codecs start in `fujinet-nio-lib` or the new
  driver repository's `common/` directory.
- Define the channel/session interface.

### Milestone 2: typed client and standalone device tests

- Add typed Mount/Info/Read/Write client helpers.
- Test exact request and response payloads.
- Build a minimal Amiga device test that reads known blocks without mounting
  AmigaDOS yet.
- Exercise the same test over RS-232 hardware and TCP/Amiberry where possible.

### Milestone 3: AmigaDOS integration

- Add the driver to a bootable test ADF.
- Add the Kickstart 1.3-compatible MountList entry.
- Mount `DN0:`.
- Run `Dir`, `Type`, and a known-file read.
- Add a write/reread test if write support is enabled in the first driver.

### Milestone 4: faster channel

- Keep the driver and DiskDevice codec unchanged.
- Add the Zorro/parallel channel backend.
- Decide whether it carries SLIP-framed byte streams or native FujiBus
  packets.
- Measure throughput, latency, queue depth, and error recovery.

## Items needing explicit agreement

Before either side commits to a large implementation, please confirm:

1. Is `fujinet-nio-driver` the preferred home for the Amiga device, with the
   current MS-DOS driver moved underneath it?
2. Should typed DiskDevice client codecs live in `fujinet-nio-lib`, in the
   driver's `common/` directory, or be split into a small shared package?
3. Is RS-232 acceptable as a correctness path even though it is not the target
   performance path?
4. Is the first Amiga acceptance profile a standard 880 KiB/1760-block ADF?
5. Is explicit unmount/remount sufficient for the first phase, with hot swap
   deferred?
6. Should the first driver be read-only, or should it support complete-block
   writes from the start?
7. For the faster channel, do we initially retain SLIP for reuse or define a
   native packet framing layer beneath the same FujiBus packets?

## Conclusion

The NIO and Amiga plans are aligned at the important architectural boundary.
NIO is taking on the generic ADF-backed block service and its stable
DiskDevice protocol. The Amiga side should take on the native Exec device,
MountList integration, AmigaDOS-facing behavior, and channel backends.

The safest next step is therefore joint agreement on the typed client and
channel interfaces, followed by a minimal Amiga device implementation. That
lets work begin now using RS-232/TCP for correctness while preserving a clean
path to a faster Zorro or parallel transport later.
