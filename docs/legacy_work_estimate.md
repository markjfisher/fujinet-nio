# Legacy Atari Work Estimate

This note captures the current estimate for making unmodified Atari FujiNet
applications work against a fujinet-nio build that uses the legacy SIO transport
instead of FujiBus.

The short version: this is medium effort, not a rewrite. The old broad
architecture work in `legacy_transport_todo.md` is mostly complete, but real
ESP32 SIO hardware and several device-level compatibility adapters remain.

## Current State

Already present:

- `LegacyTransport` plus `ByteBasedLegacyTransport` for SIO-style legacy buses
- `SioTransport`, which converts SIO command frames into `IORequest`s
- POSIX NetSIO bus hardware in `src/platform/posix/legacy/netsio_bus_hardware.cpp`
- A routing-layer legacy network adapter from `0x71..0x78` to `NetworkService` (`0xFD`)
- New core devices for network, disk, clock, and modem
- NetworkDevice JSON translation support through the newer Open extension fields

Still important:

- `src/platform/esp32/legacy/sio_bus_hardware.cpp` is still a placeholder. It
  never reports CMD asserted, reads no bytes, writes no bytes, and does not yet
  configure ESP32 GPIO/UART/timing.
- The SIO transport converts bus frames into `IORequest`s, but old Atari
  applications also depend on old device command contracts. New NIO service
  devices are not automatically wire-compatible with those old contracts.

## Effort By Area

### ESP32 SIO Bus Layer

Estimated effort: 1-2 solid weeks, including real hardware debugging.

Required work:

- Configure CMD/INT/MTR GPIOs
- Configure SIO UART pins, baud rate, and signal behavior
- Implement accurate microsecond delays for SIO timing
- Read 5-byte command frames when CMD is asserted
- Read/write data frames with checksums
- Validate behavior on real Atari hardware

High-speed SIO negotiation may add more time if it must work in the first pass.

### Network `N:` Applications

Estimated effort: a few days to 1 week for the remaining gaps.

Mostly implemented:

- Legacy `O`, `C`, `R`, `W`, and `S` commands map to new NetworkDevice
  Open/Close/Read/Write/Info requests.
- One new network handle is maintained per legacy network device ID.
- Legacy POST/PUT streaming is handled by committing on first STATUS/READ.

Likely remaining work:

- Audit old JSON/channel commands and map them to current NetworkDevice
  translation extensions.
- Add compatibility tests against representative old Atari network applications.

### Disk `D:` Applications

Estimated effort: 1-2 weeks for normal ATR sector I/O; more for full
status/format/boot edge cases.

Not implemented as a legacy adapter today. New `DiskDevice` is a service
protocol on `0xFC` with versioned binary payloads. Old Atari software sends
`D1:` style SIO sector commands to `0x31..0x3F`.

Required work:

- Add a legacy disk adapter for Atari SIO disk command bytes
- Map legacy sector read/write/status/format semantics to `DiskService` or
  `DiskDevice`
- Preserve Atari sector sizing and ATR geometry behavior
- Validate boot and DOS behavior with old clients

### Clock

Estimated effort: 1-3 days if the old command formats align reasonably.

Device ID `0x45` already maps to `ClockDevice`, but legacy clock command bytes
and payload formats need an audit. Add a small adapter if the current
`ClockCommand` payloads are not legacy-compatible.

### Modem / R: Style Clients

Estimated effort: several days to 1 week for basic stream read/write/status;
more for exact R: handler behavior.

The new modem device is a `0xFB` service protocol with versioned
Read/Write/Status/Control payloads. If old Atari modem applications expect an
R: style SIO device, add a separate legacy adapter that maps that command
surface to `ModemService`.

### Fuji Control Device `0x70`

Estimated effort: several days after the old Atari command set is enumerated.

Current FujiDevice commands are compact NIO/BBC-oriented configuration and
mount commands. Old Atari config and mounting tools may need a legacy Fuji
adapter so they can configure host slots, mount disks, and boot without
recompilation.

## Suggested Path

1. Finish ESP32 SIO hardware.
2. Keep the existing `cmdFrame_t -> IORequest -> IOResponse -> SIO frame`
   pipeline.
3. Add routing adapters for legacy device protocols that do not already match
   new service devices.
4. Build a regression harness using NetSIO first.
5. Validate against real Atari hardware.

## Rough Totals

- Old Atari network applications: 2-3 weeks if physical SIO behaves.
- Broad old Atari FujiNet compatibility, including network, disk, clock, modem,
  and config/mount workflows: 4-8 weeks.

The risk is concentrated in disk/Fuji/modem protocol compatibility and hardware
validation, not in the core NIO request architecture.
