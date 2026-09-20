# Amiga Zorro Autoboot Block Device

## Status

Architectural goal / future feature.

This document describes a future autoboot-capable block-device facility for
the FujiNet NIO Amiga Zorro interface.

It is intentionally separate from the existing `fujinet-disk.device` / `DNx:`
floppy-image model.

---

## Goal

Allow an Amiga fitted with the FujiNet Zorro card to cold boot directly from a
hard-disk image supplied by FujiNet, without requiring:

- a floppy disk;
- an existing Amiga hard disk;
- a pre-installed MountList;
- `fujinet-nio.device` to have been loaded from another filesystem;
- a CONFIG program to have been run first.

The initial and preferred boot source is expected to be an HDF/RDB image stored
locally on the FujiNet SD card, for example:

    SD:/amiga/boot.hdf

The same block-device architecture should later permit other FujiNet-backed
sources, for example:

    tnfs://server/amiga/boot.hdf

provided that the backing service is available early enough during Amiga boot.

---

## Why This Is Separate From DNx

`fujinet-disk.device` and `DN0:`, `DN1:`, etc. model mounted disk images and
currently have floppy-oriented semantics.

The autoboot facility should instead expose a conventional Amiga hard-disk
style Exec device:

    fujinet-hd.device

Typical DOS-visible partitions would therefore appear as ordinary hard-disk
devices such as:

    DH0:
    DH1:
    Work:
    ...

The hard-disk device and the existing DNx devices may coexist.

This separation avoids forcing RDB partitions, hard-disk geometry, autoboot,
and partition discovery into the floppy-image abstraction.

---

## High-Level Architecture

    Amiga cold boot
          |
          v
    Zorro AutoConfig
          |
          v
    FujiNet expansion ROM / DiagArea
          |
          v
    install fujinet-hd.device
          |
          v
    FujiNet block transport
          |
          v
    RP2350 bridge
          |
          v
    ESP32-S3
          |
          +----------------------+
          |                      |
          v                      v
    SD:/amiga/boot.hdf       TNFS HDF
       preferred local       optional network
          |
          v
    raw 512-byte block view
          |
          v
    RDB / partition discovery
          |
          v
    DH0:, DH1:, ...
          |
          v
    AmigaDOS autoboot

The Amiga should see a block device.

It should not need to know whether the backing image is stored on SD, TNFS, or
another future FujiNet storage provider.

---

## Expansion ROM and Autoboot

The Zorro card should contain an AutoConfig-compatible expansion ROM with a
valid diagnostic/boot area.

During early expansion initialization the ROM should:

1. participate normally in Zorro AutoConfig;
2. expose a valid DiagArea;
3. install a resident `fujinet-hd.device`;
4. associate the device with the card's ConfigDev;
5. discover configured hard-disk units;
6. inspect their RDB structures;
7. create DOS device nodes for valid partitions;
8. register bootable partitions with expansion.library before DOS starts.

The ROM-resident code may be a small bootstrap which relocates the real device
driver into RAM rather than requiring the complete driver to execute directly
from expansion ROM.

The boot implementation must support the AmigaOS version range selected for
the product.

For V36 and later, bootable nodes can be registered using `AddBootNode()`.

For Kickstart 1.3 compatibility, the equivalent BootNode must be constructed
and queued using the pre-V36 mechanism.

Kickstart 1.3 autoboot should therefore be treated as an explicit compatibility
requirement rather than an accidental property of the newer implementation.

---

## Hard-Disk Image Format

The preferred initial format is an RDB-partitioned HDF.

Example:

    SD:/amiga/boot.hdf

FujiNet exposes this file to the Amiga as a raw block device.

The Amiga-side driver should scan the initial blocks for an `RDSK`
RigidDiskBlock and then follow the RDB partition list.

For each mountable partition it should use the partition metadata to construct
the corresponding DOS node.

This follows the normal Amiga hard-disk controller model rather than inventing
a FujiNet-specific partition format.

Using an HDF file rather than exposing the complete physical SD card has several
advantages:

- the ESP32 remains the owner of the SD/FAT filesystem;
- the Amiga receives a simple raw block abstraction;
- images can be copied, backed up, replaced, or downloaded easily;
- multiple hard-disk images can coexist on one SD card;
- the same block contract can be reused for network-backed images.

---

## Block Transport

The Zorro/RP2350 transport should carry block requests, not filesystem
operations.

Conceptually:

    READ(unit, lba, count)
    WRITE(unit, lba, count, data)
    FLUSH(unit)
    GET_GEOMETRY / GET_CAPACITY
    GET_MEDIA_STATE

The exact hardware ABI is deliberately deferred until the Zorro bridge work
defines it.

`fujinet-hd.device` should not contain SD, FAT, TNFS, or Wi-Fi logic.

The ESP32 side owns the backing-storage implementation.

---

## Local SD Boot

Local SD-backed HDF should be implemented first.

This is the simplest and most deterministic autoboot path:

    power on
      |
      v
    FujiNet firmware already running
      |
      v
    open SD:/amiga/boot.hdf
      |
      v
    service block reads
      |
      v
    Amiga boots

This path should not depend on network association or external servers and is
expected to be the normal choice for users who simply want a fast, reliable
boot disk.

Local disk boot priority should normally correspond to a conventional hard
disk.

---

## Network Boot

The same hard-disk device should eventually support a network-backed image such
as:

    tnfs://server/amiga/workbench.hdf

From the Amiga's perspective this is still the same block device.

The network-specific problem is early availability.

Before DOS starts, the FujiNet must already have enough persistent
configuration to:

- know the selected boot URI;
- know Wi-Fi credentials;
- associate with the network;
- resolve/reach the remote host;
- open the backing image;
- answer block reads within a bounded timeout.

No Amiga-side CONFIG utility can be required to establish the connection,
because the disk being accessed may contain that CONFIG utility.

A network disk may use a lower boot priority than a local disk.

Network boot failure must be bounded and recoverable. The Amiga must not remain
indefinitely stuck waiting for a server which cannot be reached.

---

## Boot-Source Policy

Boot-source selection should be configuration-driven.

Possible future policy:

    1. configured local SD boot image
    2. configured network boot image
    3. no FujiNet autoboot device

or individually configured units with explicit boot priorities.

This should not become transparent mid-I/O failover between unrelated backing
stores.

In particular, an ambiguous write to one backing store must never be silently
replayed against another.

A local recovery image may eventually be useful for configuration and network
recovery.

For example:

    SD:/amiga/recovery.hdf

could contain a minimal Workbench, CONFIG, diagnostics, and network setup tools.

---

## Device Naming

Suggested separation:

    fujinet-disk.device
        Existing floppy/image-oriented DNx support.

    fujinet-hd.device
        Autoboot-capable raw hard-disk block device.

Possible examples:

    fujinet-hd.device unit 0
        SD:/amiga/boot.hdf

    fujinet-hd.device unit 1
        tnfs://server/amiga/work.hdf

RDB partitions within a unit determine the DOS-visible partition names such as
DH0:, DH1:, Work:, etc.

---

## Error and Recovery Behaviour

The boot path must have explicit behavior for:

- backing image missing;
- invalid or absent RDB;
- network unavailable;
- network timeout;
- ESP32 unavailable;
- RP2350 reset;
- media replacement;
- failed writes;
- flush failure;
- stale responses following reset.

Read failures should return normal device errors rather than hanging the Amiga.

Write completion must never be guessed after an ambiguous failure.

The initial implementation may reasonably make network-backed autoboot
read-only until recovery semantics are proven.

---

## Performance

No artificial performance target should be specified before the real Zorro
transport is measured.

Local SD-backed block access should nevertheless avoid unnecessary copies and
round trips.

Performance measurements should distinguish:

- Zorro transfer time;
- RP2350 bridge latency;
- ESP32 request processing;
- SD block access;
- TNFS/network latency.

Correct boot and recovery behavior take priority over peak throughput.

---

## Virtual / Amiberry Testing

The architecture must support development without requiring the physical Zorro
card for every software change.

Testing should occur at several levels.

### 1. Host/unit tests

Test independently:

- RDB detection;
- partition parsing;
- block addressing;
- DOS-node parameter generation;
- error mapping;
- boot-source selection;
- block-request protocol.

These tests require no emulator.

### 2. Amiga guest tests with a virtual backend

`fujinet-hd.device` should be structured so that the Amiga-facing device logic
does not depend directly on physical Zorro register accesses.

A virtual/emulator backend can therefore service block requests from a host HDF
while exercising the real Amiga device driver and partition/bootstrap code.

This can validate:

- Exec device installation;
- OpenDevice/BeginIO behavior;
- block reads/writes;
- RDB partition discovery;
- MakeDosNode parameters;
- filesystem mounting;
- DH0:/DH1: behavior;
- normal boot filesystem activity.

### 3. Full cold-autoboot emulation

The target is to support a FujiNet expansion-board model in Amiberry/WinUAE.

The emulator-side board model should reproduce the externally visible contract
of the real board:

- AutoConfig ROM identity;
- expansion ROM / DiagArea;
- configured Zorro address range;
- registers/mailbox used by `fujinet-hd.device`;
- block request/response behavior;
- interrupts if required.

The emulated board can service requests directly from a host HDF or from a
test FujiNet service.

This permits testing the complete sequence:

    power-on
      -> AutoConfig
      -> expansion ROM
      -> fujinet-hd.device initialization
      -> RDB discovery
      -> BootNode creation
      -> AmigaDOS boot from DH0:

before physical hardware is required.

Current Amiberry supports emulated Zorro expansion boards and expansion ROMs,
but a new FujiNet board should be expected to require an emulator-side board
implementation rather than assuming an arbitrary custom Zorro ROM can simply
be selected in the GUI.

The emulator implementation must remain a model of the real hardware contract,
not a separate Amiga-only shortcut.

---

## Testing Strategy

Suggested progression:

### Phase 1 — installed device, host-backed HDF

Load `fujinet-hd.device` normally in Amiberry and prove raw HDF access and RDB
partition discovery.

No autoboot yet.

### Phase 2 — virtual autoboot

Add the FujiNet expansion ROM/board model to Amiberry and boot DH0: from a
host-backed HDF.

This validates the complete Amiga early-boot path.

### Phase 3 — physical local-SD autoboot

Run the same expansion ROM and driver on the real Zorro card using:

    SD:/amiga/boot.hdf

### Phase 4 — network-backed HDF

Replace only the backing-store provider with TNFS and validate bounded network
boot behavior.

This progression deliberately keeps the Amiga-side boot architecture constant
while replacing the transport/backing implementation underneath it.

---

## Compatibility Targets

To be decided explicitly.

Likely initial goals:

- Zorro II systems;
- A500/A500+ via compatible Zorro adapter;
- A2000;
- Zorro-II compatible operation in later big-box Amigas;
- Kickstart 2.x/3.x first if this reduces bootstrap complexity;
- Kickstart 1.3 autoboot as an explicit compatibility milestone.

The chosen Kickstart baseline affects the BootNode registration implementation
and should be settled before freezing the expansion ROM.

---

## Non-Goals

This feature does not require:

- IDE register emulation;
- SCSI command emulation on the Amiga side;
- exposing the physical FujiNet SD card directly to the Amiga;
- merging hard-disk semantics into `fujinet-disk.device`;
- making DNx devices bootable by redefining their existing model;
- implementing a filesystem in the RP2350;
- moving RDB parsing into the RP2350.

The RP2350 remains primarily the high-speed Zorro/FujiNet bridge.

---

## Architectural Principle

The central design rule is:

> The Amiga sees a normal autoboot-capable block device; FujiNet decides where
> its blocks come from.

That keeps the Amiga boot/device implementation stable while allowing the
backing storage to evolve from local SD HDF to TNFS and other FujiNet storage
providers.