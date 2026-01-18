# Legacy Transport Implementation Status

## Overview

This document tracks the implementation status of the legacy transport layer for fujinet-nio, starting with Atari SIO support.

## Completed Components

### Core Infrastructure

✅ **BusTraits Interface** (`include/fujinet/io/transport/legacy/bus_traits.h`)
- Defines bus characteristics (checksums, timing, response styles)
- Platform-agnostic - same traits for all platforms
- Single implementation in `src/lib/transport/legacy/sio_traits.cpp`

✅ **Command Frame Structure** (`include/fujinet/io/transport/legacy/cmd_frame.h`)
- Common `cmdFrame_t` structure used by legacy buses
- 5-byte frame: device, comnd, aux1, aux2, checksum

✅ **Bus Hardware Abstraction** (`include/fujinet/io/transport/legacy/bus_hardware.h`)
- Abstract interface for platform-specific hardware access
- GPIO, UART, and timing operations
- Platform-specific implementations in `src/platform/<platform>/sio_bus_hardware.cpp`

✅ **Legacy Transport Base Class** (`include/fujinet/io/transport/legacy/legacy_transport.h`)
- Common protocol logic for all legacy buses
- Handles frame parsing, conversion to/from IORequest/IOResponse
- State machine for protocol flow

### SIO Transport Implementation

✅ **SIO Transport** (`include/fujinet/io/transport/legacy/sio_transport.h`)
- Atari SIO-specific transport implementation
- Extends LegacyTransport with SIO-specific protocol handling

✅ **Platform Hardware**
- `src/platform/esp32/sio_bus_hardware.cpp` - ESP32 hardware abstraction (placeholder)
- `src/platform/posix/sio_bus_hardware.cpp` - POSIX hardware abstraction (placeholder)

✅ **Integration**
- Updated `src/lib/bootstrap.cpp` to register SIO transport
- SIO transport is now available when `TransportKind::SIO` is selected

## Implementation Details

### SIO Protocol Flow

1. **Command Frame Reception**
   - Wait for CMD pin assertion (GPIO)
   - Read 5-byte command frame from UART
   - Validate checksum using SIO algorithm
   - Send ACK or NAK

2. **Data Frame Handling** (if needed)
   - Read data frame with checksum
   - Validate checksum
   - Send ACK or NAK

3. **Response**
   - Send COMPLETE or ERROR
   - Write data frame with checksum (if response has payload)

### Device ID Mapping

SIO wire device IDs are mapped to internal DeviceIDs:
- `0x70` → FujiNet device
- `0x31-0x3F` → Disk devices (pass-through)
- `0x71-0x78` → Network devices (pass-through)
- `0x40-0x43` → Printer devices (pass-through)
- `0x45` → Clock device
- `0x99` → MIDI device

### Timing Constants

- `DELAY_T4 = 850` microseconds (delay before reading checksum)
- `DELAY_T5 = 250` microseconds (delay before sending COMPLETE/ERROR)

## Remaining Work

### Hardware Implementation

⚠️ **ESP32 Hardware** (`src/platform/esp32/sio_bus_hardware.cpp`)
- Currently a placeholder
- Needs GPIO pin configuration for:
  - CMD pin (input)
  - INT pin (output)
  - MTR pin (input, optional)
- Needs UART configuration for SIO bus
- Needs timing functions (delayMicroseconds)

⚠️ **POSIX Hardware** (`src/platform/posix/sio_bus_hardware.cpp`)
- Currently a placeholder
- Needs integration with NetSIO or serial port
- For NetSIO, command assertion is protocol-based, not GPIO

### Protocol Refinements

- [ ] Handle Type 3 polls (broadcast device ID `0x7F`)
- [ ] Implement high-speed SIO negotiation
- [ ] Handle special commands (e.g., `0x3F` for HSIO index)
- [ ] Support for multiple data frames in a single transaction

### Testing

- [ ] Unit tests for BusTraits and frame parsing
- [ ] Integration tests with mock hardware
- [ ] Hardware tests with real Atari hardware

## Usage

To use the SIO transport, set the build profile's `primaryTransport` to `TransportKind::SIO`:

```cpp
build::BuildProfile profile;
profile.machine = build::Machine::Atari8Bit;
profile.primaryTransport = build::TransportKind::SIO;
```

The transport will be automatically registered via `core::setup_transports()`.

## Architecture Notes

### Design Decisions

1. **Channel vs Hardware**: The `LegacyTransport` base class takes a `Channel&` parameter to satisfy the `ITransport` interface, but SIO uses `BusHardware` directly for GPIO/UART access. The Channel parameter may be unused for SIO but is required by the interface.

2. **Platform-Specific Code**: All platform-specific code (GPIO, UART, timing) is isolated in `src/platform/<platform>/legacy/` directories. No `#ifdef`s in core transport code.

3. **Traits Pattern**: Bus-specific behaviors (checksums, timing, response styles) are captured in `BusTraits` registries, allowing the same transport code to work across platforms with different hardware characteristics.

## Next Steps

1. **Complete Hardware Implementation**
   - Implement ESP32 GPIO/UART access
   - Implement POSIX NetSIO integration
   - Add proper timing functions

2. **Test with Real Hardware**
   - Connect to Atari 8-bit computer
   - Test basic SIO commands
   - Verify checksum validation
   - Test timing-sensitive operations

3. **Extend to Other Platforms**
   - IWM (Apple II)
   - IEC (Commodore)
   - AdamNet (Coleco Adam)
