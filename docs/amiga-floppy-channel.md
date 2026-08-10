# Amiga Floppy-Port FujiNet Channel

This document records a proposed use of the Amiga floppy connector as a
FujiNet channel. It is a design note, not yet a hardware or wire-protocol
contract. PaulaNET is approach-level reference material only: it demonstrates
that useful data movement can be built around the floppy interface, but it is
not a source of reusable code or hardware.

Related documents:

- [`driver_architecture.md`](driver_architecture.md) — driver, FujiBus, SLIP,
  and channel boundaries.
- [`disk_device_protocol.md`](disk_device_protocol.md) — generic NIO
  DiskDevice commands and ADF block behavior.
- [`architecture.md`](architecture.md) — NIO channel and transport layering.
- [`posix_tcp_serial_channel.md`](posix_tcp_serial_channel.md) — TCP as a
  development/emulator channel.

## The idea in one sentence

Use a Pico microcontroller as a real-time adapter between the Amiga floppy
connector and an ESP32 running FujiNet-NIO, while keeping the existing FujiBus
and DiskDevice protocol above that adapter.

The Pico is not the disk server. It is the translator between two very
different electrical/data worlds:

```text
Amiga floppy signalling  <->  Pico  <->  local board link  <->  ESP32/NIO
```

The ESP32 is the part that provides Wi-Fi/network access and runs the FujiNet
services. The Pico does not need Wi-Fi in the integrated product.

## Why the Pico is useful

The Amiga floppy connector is not an ordinary serial port. It carries floppy
drive control signals and a Paula/DMA-driven MFM data stream. A general-purpose
Amiga CPU driver can request raw track operations, but reliably converting
those operations into and out of the floppy electrical/timing protocol is a
specialized real-time job.

The Pico is the proposed adapter in this design:

- samples and drives the floppy-port signals;
- handles timing-sensitive MFM/track data;
- buffers data between the Amiga and FujiNet;
- detects packet boundaries and validates transfers;
- applies channel flow control and retry rules;
- forwards FujiBus packets to the ESP32;
- returns FujiBus responses to the Amiga.

In plain terms, the Pico is a fast translator and buffer. It does not decide
which disk sector AmigaDOS wants and it does not open an ADF file. Those jobs
belong elsewhere.

## Responsibility split

```text
AmigaDOS / fujinet-disk.device
    Understands Amiga device requests and filesystem-facing behavior.
    Decides that a particular logical block must be read or written.

Pico floppy adapter
    Understands Paula/floppy electrical signalling and timing.
    Moves FujiBus packets across the floppy connector and local link.

ESP32 FujiNet-NIO
    Provides network access and runs FujiNet-NIO.
    Receives FujiBus requests and routes them to DiskDevice/DiskService.

NIO DiskService
    Mounts ADF/raw images and reads/writes numbered 512-byte blocks.
```

For example, a block read travels like this:

```text
AmigaDOS asks the device for block 1759
        |
fujinet-disk.device builds DiskDevice ReadSector(slot=1, lba=1759)
        |
Pico moves the FujiBus request through the floppy-port channel
        |
ESP32 receives the request through its local channel adapter
        |
NIO DiskDevice reads block 1759 from the mounted ADF
        |
The response returns through the same path
```

The Pico does not generate `ReadSector` on its own. The Amiga driver generates
the logical request; the Pico transports it.

## Integrated hardware design

The intended combined device is:

```text
Amiga floppy connector
          |
          v
      Pico 2 W
          |
   SPI / UART / similar
          |
          v
       ESP32
          |
     Wi-Fi / network
          |
          v
    FujiNet-NIO services
```

The exact Pico-to-ESP32 bus is still open. SPI is a strong candidate for the
integrated board because it provides a clocked, high-throughput connection
with relatively few pins. UART is simpler and may be sufficient for early
experiments. GPIO should primarily be used for control signals such as
interrupt/data-ready, reset, and flow control rather than as an unclocked
bulk-data bus.

The local bus should carry FujiBus packets, or a small envelope containing a
FujiBus packet:

```text
length | sequence/flags | FujiBus packet | CRC
```

The envelope is useful for packet boundaries, integrity checking, and
recovery on SPI or UART. It must not create a second disk command protocol.

The ESP32 side would expose a channel implementation to NIO roughly like:

```text
Pico local-link adapter
        |
        v
ESP32 Channel implementation
        |
        v
FujiBusTransport
        |
        v
DiskDevice -> DiskService
```

The NIO disk service remains unaware that the request arrived through a
floppy connector.

## Why the integrated channel will not use SLIP

FujiBus and SLIP remain separate concepts, as described in
[`driver_architecture.md`](driver_architecture.md):

- FujiBus is the logical request/response packet protocol.
- SLIP is the existing framing used by legacy byte-stream channels.
- The floppy connector needs its own timing/data representation because it is
  an MFM/DMA interface.

The integrated floppy-port design should use packet-native FujiBus transport.
It will not use SLIP between the Pico and ESP32. The local link should carry a
FujiBus packet inside a small packet envelope, for example:

```text
length | sequence/flags | FujiBus packet | CRC
```

The envelope supplies packet boundaries, integrity checking, and recovery
metadata for SPI or UART. It is a channel envelope, not a second disk command
protocol. The ESP32 removes the envelope and passes the FujiBus packet into
the normal NIO request path.

SLIP remains relevant only to compatibility/development paths that require a
raw byte stream, such as the current TCP/PTY/RS-232 FujiBus paths. It is not the
target framing for the integrated floppy/Pico/ESP32 device.

## Development-board configuration

There are two possible development arrangements.

### Pico directly using TCP (optional convenience)

```text
Amiga floppy connector -> Pico 2 W -> Wi-Fi/TCP -> POSIX FujiNet-NIO
```

This is useful because it allows floppy-port hardware and Amiga-driver work
before the ESP32 integration exists. In this arrangement the Pico 2 W does
use its wireless capability as a temporary development link. It is not the
integrated-product architecture and should not determine the Pico/ESP32 link
API.

### Pico connected to a host bridge (preferred development fallback)

```text
Amiga floppy connector -> Pico 2 W -> USB/UART -> host bridge -> TCP -> POSIX NIO
```

This avoids putting Wi-Fi code in the Pico firmware. A small host-side bridge
converts the Pico packet link over USB/UART to a TCP connection to POSIX NIO.
This is the preferred development arrangement if the Pico should remain a
floppy/link controller only.

### Integrated product

```text
Amiga floppy connector -> Pico -> SPI/UART -> ESP32/NIO -> Wi-Fi/network
```

Here the Pico does not do Wi-Fi. The ESP32 owns all network connectivity and
FujiNet-NIO execution. The Pico and ESP32 use the same packet-oriented
FujiNetLink interface that the optional host bridge can expose during
development.

The recommended software abstraction is therefore:

```text
Pico FujiNetLink
    - HostBridgeLink       (preferred development backend)
    - TcpFujiNetLink       (optional Pico-Wi-Fi convenience backend)
    - Esp32LocalLink       (integrated hardware backend)
```

The floppy/MFM side of the Pico remains unchanged across these backends.

## What “pure hardware” would mean

A pure-hardware design would replace some or all Pico firmware with FPGA or
dedicated logic that handles floppy timing, packet buffering, and the local
link. That could eventually reduce latency or simplify a production board,
but it would make early protocol changes, diagnostics, and recovery behavior
much harder.

The Pico is valuable precisely because the channel is still being explored:

- its firmware can change as the floppy protocol is understood;
- buffering and compression can be tested;
- packet traces and diagnostics can be added;
- TCP, USB, SPI, and UART backends can be compared;
- the same hardware can support development and integrated-board testing.

Pure hardware should be treated as a later optimization, not a prerequisite
for the first driver or channel prototype.

## Relationship to the Amiga driver

The Amiga driver remains channel-independent above the Pico link. It should
see an abstract FujiNet packet/session interface rather than floppy registers,
Wi-Fi, TCP, SPI, or UART.

```text
same fujinet-disk.device
        |
        +-- RS-232 channel backend
        +-- Pico floppy channel backend
        +-- TCP/Amiberry backend
        +-- future Zorro backend
```

The Pico floppy backend is not a second disk driver. It is a transport path
for the same Mount, Info, ReadSector, WriteSector, and related DiskDevice
operations documented in [`disk_device_protocol.md`](disk_device_protocol.md).

## Suggested implementation order

1. Study PaulaNET’s approach at a high level. It is primarily a standard
   floppy-drive emulator using the Amiga’s normal trackdisk path, with
   networking carried through reserved tracks. Its reported throughput and
   track allocation should not be treated as FujiNet channel requirements.
   Its implementation and hardware files are reference material only; do not
   reuse them without permission.
2. Define a small Pico-to-endpoint packet envelope.
3. Implement Pico floppy receive/transmit and local packet buffering.
4. Add a temporary TCP or host-bridge backend for POSIX NIO.
5. Exercise FujiBus packets without involving AmigaDOS first.
6. Add the ESP32 local-link backend, initially using SPI or UART.
7. Run the same Amiga disk-driver tests through the integrated board.
8. Measure throughput, latency, retries, buffer depth, and link recovery.

## Open decisions

- SPI or UART for the Pico-to-ESP32 local bus?
- Packet envelope format and CRC?
- Which side owns retries and sequence numbers?
- Is the first development backend Pico Wi-Fi TCP or USB/host bridging?
- What packet envelope, CRC, sequencing, and flow control does the ESP32 local
  channel use for complete FujiBus packets?
- How are Pico and ESP32 reset and firmware-version mismatches handled?
- What level shifting, open-collector behavior, protection, and power does the
  floppy connector require?
- Can the Pico expose enough buffering to keep Paula DMA and the ESP32 link
  decoupled?

The product boot path is also separate from the driver transport question. A
device connected only to the floppy connector cannot use expansion-bus
autoboot; a floppy-emulation boot volume may instead load the driver and have
its Startup-Sequence mount the larger DiskDevice volume. That dual floppy/
hard-disk direction is still a product-level trade study and does not change
the driver’s channel-independent contract.

## Summary

The Pico is not a replacement for FujiNet-NIO and not a second disk server. It
is the real-time physical adapter that lets an Amiga floppy connector become a
FujiNet channel.

The integrated product should put Wi-Fi and FujiNet-NIO on the ESP32, with the
Pico handling floppy timing and a fast local link. A TCP-capable Pico setup is
useful as a development shortcut, but it is only one implementation of the
Pico’s downstream FujiNet link. The Amiga driver and NIO DiskDevice protocol
remain unchanged above that link.
