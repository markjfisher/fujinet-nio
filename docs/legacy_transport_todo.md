# Legacy Transport Implementation TODO

## Overview

This document tracks the remaining work for implementing legacy bus protocol support in fujinet-nio. The SIO (Atari) and IWM (Apple II) transports have been created with placeholder hardware implementations.

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

## Remaining Work

### Phase 1: Hardware Implementation

#### SIO Hardware (Atari)

**ESP32** (`src/platform/esp32/sio_bus_hardware.cpp`):
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

**POSIX** (`src/platform/posix/sio_bus_hardware.cpp`):
- [x] Integrate with NetSIO protocol (UDP):
  - Command assertion via protocol (not GPIO)
  - UDP channel + NetSIO message parsing (`src/platform/posix/netsio_bus_hardware.cpp`)
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

### Phase 3: Code Refactoring

#### Remove Duplication

- [ ] **Analyze ACK/NAK/COMPLETE/ERROR patterns**:
  - SIO uses these control bytes
  - IWM doesn't use them (packet-based)
  - Consider making them optional in LegacyTransport base class
  - Or create separate base classes for byte-based vs packet-based protocols

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

- [ ] Consider protocol-specific base classes:
  - `ByteBasedLegacyTransport` (SIO, IEC)
  - `PacketBasedLegacyTransport` (IWM)
  - Share common logic in `LegacyTransport` base

- [ ] Improve hardware abstraction:
  - Separate GPIO operations from serial operations
  - Create `GpioHardware` and `SerialHardware` interfaces
  - Compose as needed (SIO needs both, IWM needs SPI+GPIO)

### Phase 4: Testing

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

### Phase 5: Python Client Updates

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

1. **Immediate**: Implement ESP32 SIO hardware (most critical for Atari support)
2. **Short-term**: Add unit tests for existing code
3. **Medium-term**: Implement IWM packet encoding/decoding
4. **Long-term**: Hardware testing and Python client updates

## Notes

- IWM's packet-based protocol suggests we may need to refactor `LegacyTransport` to support both byte-based (SIO) and packet-based (IWM) protocols more cleanly.
- The ACK/NAK/COMPLETE/ERROR methods in LegacyTransport are SIO-specific and should be made optional or moved to a SIO-specific base class.
- Hardware implementations are currently placeholders and need platform-specific GPIO/SPI/UART code.
