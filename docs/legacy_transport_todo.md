# Legacy Transport Implementation TODO

## Overview

This document tracks remaining legacy bus work in fujinet-nio. The current
priority is Atari SIO compatibility for unmodified legacy FujiNet applications.
Apple II/IWM notes are retained for context, but are not part of the Atari-only
work estimate.

For effort and scope, see `legacy_work_estimate.md`.

## Completed ✅

1. ✅ **Core Infrastructure**
   - BusTraits interface and SIO/IWM traits
   - Command frame structure (cmdFrame_t)
   - Bus hardware abstraction interface
   - LegacyTransport base class
   - SioTransport implementation
   - IwmTransport implementation
   - Integration into bootstrap.cpp
   - NetSIO protocol parsing + POSIX `NetSioBusHardware` (UDP)
   - Routing-layer legacy network adapter (`0x71..0x78` → `0xFD`)
   - Byte-based data-phase reads use protocol-correct expected lengths (prevents legacy WRITE hangs)
   - Protocol-specific legacy base classes exist:
     - `ByteBasedLegacyTransport` for SIO-style ACK/NAK/COMPLETE/ERROR flows
     - `PacketBasedLegacyTransport` for packet-style flows

## Remaining Work

### Phase 1: Hardware Implementation

#### SIO Hardware (Atari)

**ESP32** (`src/platform/esp32/legacy/sio_bus_hardware.cpp`):
- [ ] Configure GPIO pins:
  - CMD pin (input) - command line assertion
  - INT pin (output) - interrupt line
  - MTR pin (input, optional) - motor line for cassette
- [ ] Configure UART for SIO bus:
  - Pin assignment (TX/RX)
  - Baud rate (19200 standard, variable high-speed)
  - Inverted signals if needed
- [ ] Implement timing functions:
  - `delayMicroseconds()` using ESP32 timer
  - Critical delays: T4 (850us), T5 (250us)
- [ ] Implement command frame reading:
  - Wait for CMD pin assertion
  - Read 5-byte command frame
  - Wait for CMD de-assertion
- [ ] Implement data frame I/O:
  - Read data + checksum, validate
  - Write data + checksum

**POSIX** (`src/platform/posix/legacy/sio_bus_hardware.cpp`):
- [x] Integrate with NetSIO protocol (UDP):
  - Command assertion via protocol (not GPIO)
  - UDP channel + NetSIO message parsing (`src/platform/posix/legacy/netsio_bus_hardware.cpp`)
- [ ] Or integrate with physical serial port:
  - Open serial device
  - Configure baud rate, parity, etc.
  - Handle RS-232 control lines if available

#### IWM Hardware (Apple II)

**ESP32** (`src/platform/esp32/iwm_bus_hardware.cpp`):
- [ ] Configure SPI for IWM bus:
  - SPI mode, speed, pin assignment
  - SPI transaction handling
- [ ] Configure GPIO phase lines:
  - PH0, PH1, PH2, PH3 (inputs)
  - Read phase patterns to detect bus state
- [ ] Configure GPIO control lines:
  - REQ line (input) - request line
  - ACK line (output) - acknowledge line
- [ ] Implement phase detection:
  - Reset: PH3=0, PH2=1, PH1=0, PH0=1 (0b0101)
  - Enable: PH3=1, PH1=1 (0b1X1X)
  - Idle: other patterns
- [ ] Implement SPI packet I/O:
  - Read SPI packets (with sync byte detection)
  - Write SPI packets (with encoding)
  - Handle timing-critical operations

**POSIX** (`src/platform/posix/iwm_bus_hardware.cpp`):
- [ ] Integrate with SLIP relay protocol:
  - Command/data packets via SLIP
  - Phase state emulation
- [ ] Or implement emulation mode:
  - Protocol-level phase simulation
  - Packet encoding/decoding

### Phase 2: Protocol Implementation

#### IWM Packet Encoding/Decoding

- [ ] Implement IWM packet encoding (`encode_packet`):
  - Sync bytes (0xFF, 0x3F, 0xCF, 0xF3, 0xFC, 0xFF)
  - Header encoding (PBEGIN, DEST, SRC, TYPE, AUX, STAT)
  - Data encoding (groups of 7 bytes + odd bytes)
  - Checksum calculation (XOR-based)
  - Packet end marker (0xC8)
- [ ] Implement IWM packet decoding (`decode_packet`):
  - Sync byte detection (0xC3)
  - Header parsing
  - Data decoding (groups of 7 + odd bytes)
  - Checksum validation
- [ ] Handle extended commands (0x40-0x4F range)
- [ ] Handle variable-length packets (up to 767 bytes)

#### IWM Phase State Machine

- [ ] Implement phase state detection:
  - Continuous monitoring of phase lines
  - State transitions (idle → reset → enable)
  - Timeout handling
- [ ] Implement reset handling:
  - Clear all device addresses
  - Wait for reset de-assertion
  - Sample /EN35 line (3.5" drive support)
- [ ] Implement enable handling:
  - Read command packet via SPI
  - Decode command packet
  - Route to appropriate device

#### IWM INIT Command

- [ ] Implement INIT command handling:
  - Assign device IDs dynamically
  - Check for devices in daisy chain
  - Send INIT reply packets
  - Handle last device in chain (status bit)

### Phase 3: Atari Device Compatibility Adapters

The SIO transport converts command frames into `IORequest`s, but old Atari
software does not automatically speak the new service protocols. Each legacy
device surface must either already match the new device command contract or get
a routing adapter.

#### Network (`N:` / `0x71..0x78`)

- [x] Route old network device IDs to `NetworkService` (`0xFD`)
- [x] Map legacy `O`, `C`, `R`, `W`, `S` commands to new Open/Close/Read/Write/Info requests
- [x] Maintain one new network handle per legacy device ID
- [x] Support legacy POST/PUT streaming by committing on first STATUS/READ
- [ ] Audit old JSON/channel commands and map them to current `NetworkDevice` translation extensions
- [ ] Add compatibility tests for representative old Atari `N:` applications

#### Disk (`D1:`.. / `0x31..0x3F`)

- [ ] Add a legacy disk adapter for Atari SIO disk command bytes
- [ ] Map legacy sector read/write/status/format semantics onto `DiskService`/`DiskDevice`
- [ ] Preserve Atari sector sizing and ATR geometry behavior
- [ ] Validate boot and DOS edge cases against real old Atari clients

#### Clock (`0x45`)

- [ ] Audit legacy clock command bytes and payload formats against `ClockDevice`
- [ ] Add a small adapter if legacy payloads do not match current `ClockCommand` formats

#### Modem / R: style workflows

- [ ] Decide the legacy SIO device ID and command surface to support
- [ ] Add an adapter to `ModemService` (`0xFB`) if old Atari modem clients expect R:-style SIO reads/writes/status
- [ ] Validate Hayes/AT command and stream behavior with existing old clients

#### Fuji control device (`0x70`)

- [ ] Enumerate legacy Atari FujiDevice config/mount commands
- [ ] Map old mount/config workflows to current `FujiDevice`, `DiskService`, and config storage
- [ ] Verify old mounting tools can configure and boot disks without recompilation

### Phase 4: Code Refactoring

Most of the original architecture refactor is now complete. Keep this section
for smaller cleanup only.

#### Remove Duplication

- [x] **Analyze ACK/NAK/COMPLETE/ERROR patterns**:
  - SIO uses these control bytes
  - IWM doesn't use them (packet-based)
  - Implemented as separate byte-based and packet-based base classes

- [ ] **Extract common frame parsing**:
  - Both SIO and IWM parse command frames
  - SIO: simple 5-byte frame
  - IWM: complex SPI-encoded packet
  - Share validation logic where possible

- [ ] **Common device ID mapping**:
  - Both need wire ID → internal DeviceID mapping
  - SIO: fixed mapping
  - IWM: dynamic assignment
  - Extract mapping logic to shared helper

#### Improve Architecture

- [x] Consider protocol-specific base classes:
  - `ByteBasedLegacyTransport` (SIO, IEC)
  - `PacketBasedLegacyTransport` (IWM)
  - Share common logic in `LegacyTransport` base

- [ ] Improve hardware abstraction:
  - Separate GPIO operations from serial operations
  - Create `GpioHardware` and `SerialHardware` interfaces
  - Compose as needed (SIO needs both, IWM needs SPI+GPIO)

### Phase 5: Testing

#### Unit Tests

- [ ] **BusTraits tests**:
  - Test SIO checksum algorithm
  - Test IWM checksum algorithm
  - Test device ID mapping functions
  - Test timing constants

- [ ] **LegacyTransport tests**:
  - Test cmdFrame_t → IORequest conversion
  - Test IOResponse → legacy format conversion
  - Test state machine transitions
  - Test checksum validation

- [ ] **SioTransport tests**:
  - Test command frame reading (mock hardware)
  - Test ACK/NAK/COMPLETE/ERROR sending
  - Test data frame I/O with checksum
  - Test command needs data detection

- [ ] **IwmTransport tests**:
  - Test phase detection (mock GPIO)
  - Test packet encoding/decoding
  - Test INIT command handling
  - Test status/data packet sending

#### Integration Tests

- [ ] **SIO integration tests** (Python):
  - Create test harness for SIO protocol
  - Send SIO command frames
  - Verify responses (ACK, COMPLETE, data)
  - Test checksum validation
  - Test timing-sensitive operations

- [ ] **IWM integration tests** (Python):
  - Create test harness for IWM protocol
  - Send IWM command packets (SPI-encoded)
  - Verify responses (status packets, data packets)
  - Test phase state transitions
  - Test INIT sequence

#### Hardware Tests

- [ ] **SIO with real Atari hardware**:
  - Connect to Atari 8-bit computer
  - Test basic SIO commands (STATUS, READ, WRITE)
  - Verify timing (T4, T5 delays)
  - Test checksum validation
  - Test high-speed SIO negotiation

- [ ] **IWM with real Apple II hardware**:
  - Connect to Apple II computer
  - Test phase detection
  - Test INIT sequence
  - Test SmartPort commands
  - Verify packet encoding/decoding

### Phase 6: Python Client Updates

#### Legacy Transport Support

- [ ] **Add legacy transport modes**:
  - SIO mode (Atari)
  - IWM mode (Apple II)
  - Detect transport type from connection

- [ ] **Legacy frame encoding/decoding**:
  - SIO: cmdFrame_t encoding/decoding
  - IWM: SPI packet encoding/decoding
  - Helper functions for both

- [ ] **Legacy protocol helpers**:
  - `send_sio_command()` - send SIO command frame
  - `send_iwm_command()` - send IWM command packet
  - `read_sio_response()` - read SIO response
  - `read_iwm_response()` - read IWM response

- [ ] **Update existing tools**:
  - Network device tools (support legacy SIO/IWM)
  - Disk device tools (support legacy SIO/IWM)
  - File device tools (support legacy SIO/IWM)

#### Test Utilities

- [ ] **Legacy protocol test scripts**:
  - `test_sio_protocol.py` - SIO protocol tests
  - `test_iwm_protocol.py` - IWM protocol tests
  - Verify compatibility with old firmware

## Key Findings from IWM Implementation

### Differences from SIO

1. **No control bytes**: IWM doesn't use ACK/NAK/COMPLETE/ERROR bytes. It's entirely packet-based.
2. **Phase-based protocol**: IWM uses GPIO phase lines to determine bus state, not time-based like SIO.
3. **SPI vs UART**: IWM uses SPI for data transfer, SIO uses UART.
4. **Dynamic device IDs**: IWM assigns device IDs during INIT, SIO uses fixed IDs.
5. **Complex packet encoding**: IWM encodes data in groups of 7 bytes with XOR checksum, SIO uses simple additive checksum.

### Common Patterns

1. **Command frame structure**: Both use similar cmdFrame_t structure (though IWM encodes it differently).
2. **Device abstraction**: Both use VirtualDevice pattern.
3. **Hardware abstraction**: Both benefit from BusHardware interface.
4. **IORequest/IOResponse**: Both convert to/from the same core message types.

## Next Steps

1. **Immediate**: Implement ESP32 SIO hardware (most critical for real Atari support)
2. **Short-term**: Add Atari-focused legacy adapters for disk/Fuji control gaps
3. **Short-term**: Add tests for existing SIO and legacy network adapter behavior
4. **Medium-term**: Validate against NetSIO first, then real Atari hardware
5. **Later**: Return to IWM packet encoding/decoding when Apple II becomes active work

## Notes

- The old broad architecture notes are stale: byte-based and packet-based legacy base classes now exist.
- ESP32 SIO hardware remains a placeholder and needs platform-specific GPIO/UART/timing code.
- POSIX NetSIO is implemented; POSIX physical serial SIO remains optional/future work.
