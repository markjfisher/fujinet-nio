# Legacy Transport Design

## Overview

This document describes the architecture for supporting legacy bus protocols (SIO, IWM, IEC, AdamNet, etc.) in fujinet-nio. These transports allow old applications that communicate with legacy fujinet-firmware to work with the new fujinet-nio firmware without modification.

## Core Principles

1. **Platform-agnostic core**: All legacy transport logic lives in `include/fujinet/io/transport/legacy/` and `src/lib/transport/legacy/`
2. **Platform-specific traits**: Bus-specific behaviors (checksums, timing, framing) are expressed via registries/factories, not `#ifdef`s
3. **Clean conversion**: Legacy command frames → `IORequest` → `IOResponse` → legacy response frames
4. **No duplication**: Each platform's unique traits are captured once in a registry, not copied across devices

## Architecture

### 1. Transport Layer Structure

The legacy transport architecture uses a three-tier inheritance hierarchy to cleanly separate protocol styles:

```
fujinet::io::ITransport (abstract)
└── LegacyTransport (abstract base)
    ├── ByteBasedLegacyTransport (SIO, IEC)
    │   └── SioTransport (Atari)
    └── PacketBasedLegacyTransport (IWM)
        └── IwmTransport (Apple II)
```

**LegacyTransport** provides common functionality:
- `poll()` - reads raw bytes from channel
- `convertToIORequest()` - converts cmdFrame_t to IORequest
- State machine management
- Abstract methods: `receive()`, `send()`, `readCommandFrame()`

**ByteBasedLegacyTransport** handles protocols with explicit control bytes:
- Implements `receive()` with ACK/NAK flow control
- Implements `send()` with COMPLETE/ERROR + data
- Requires: `sendAck()`, `sendNak()`, `sendComplete()`, `sendError()`, `readDataFrame()`, `writeDataFrame()`

**PacketBasedLegacyTransport** handles packet-based protocols:
- Implements `receive()` for packet-based protocols
- Implements `send()` with status packet + data packet
- Requires: `sendStatusPacket()`, `sendDataPacket()`, `readDataPacket()`

This separation ensures each protocol only implements what it actually uses, avoiding no-op methods.

### 2. Command Frame Abstraction

Each legacy bus uses a `cmdFrame_t` structure, but they differ slightly:

**Common structure:**
```cpp
struct cmdFrame_t {
    uint8_t device;   // Device ID on the bus
    uint8_t comnd;    // Command byte
    uint8_t aux1;     // Auxiliary byte 1
    uint8_t aux2;     // Auxiliary byte 2
    uint8_t checksum; // Frame checksum
};
```

**Platform variations:**
- **SIO (Atari)**: Standard 5-byte frame, checksummed
- **IWM (Apple II)**: Similar structure but different checksum algorithm
- **IEC (Commodore)**: Different framing (IEC bus protocol)
- **AdamNet**: XOR checksum instead of additive

### 3. Platform-Specific Traits Registry

Each platform's unique bus characteristics are captured in a registry:

```cpp
namespace fujinet::platform::legacy {

struct BusTraits {
    // Checksum algorithm
    using ChecksumFn = std::function<uint8_t(const uint8_t*, size_t)>;
    ChecksumFn checksum;
    
    // Timing constants (microseconds)
    uint32_t ack_delay;
    uint32_t complete_delay;
    uint32_t error_delay;
    
    // Response protocol
    enum class ResponseStyle {
        AckNakThenData,      // SIO: ACK/NAK, then COMPLETE/ERROR + data
        StatusThenData,      // IWM: Status byte, then data packet
        ImmediateData,       // IEC: Data immediately, no status
    };
    ResponseStyle response_style;
    
    // Frame validation
    bool validate_checksum(const cmdFrame_t& frame) const;
    
    // Device ID mapping (wire → internal DeviceID)
    DeviceID map_device_id(uint8_t wire_id) const;
};

// Platform-specific factories
BusTraits make_sio_traits();
BusTraits make_iwm_traits();
BusTraits make_iec_traits();
BusTraits make_adamnet_traits();

} // namespace fujinet::platform::legacy
```

### 4. Legacy Command Frame Parser

Platform-agnostic parser that uses traits:

```cpp
namespace fujinet::io::transport::legacy {

class LegacyFrameParser {
public:
    explicit LegacyFrameParser(const platform::legacy::BusTraits& traits)
        : _traits(traits) {}
    
    // Try to parse a complete command frame from buffer
    // Returns true if a valid frame was found
    bool tryParse(const std::vector<uint8_t>& buffer, cmdFrame_t& out);
    
    // Convert cmdFrame_t → IORequest
    IORequest toIORequest(const cmdFrame_t& frame, RequestID id);
    
private:
    const platform::legacy::BusTraits& _traits;
};

} // namespace fujinet::io::transport::legacy
```

### 5. Legacy Transport Base Classes

The architecture uses three base classes to handle different protocol styles:

**LegacyTransport** (most abstract):
```cpp
namespace fujinet::io::transport::legacy {

class LegacyTransport : public ITransport {
public:
    explicit LegacyTransport(Channel& channel, const BusTraits& traits);
    
    void poll() override;
    
    // Pure virtual - protocol-specific implementations
    virtual bool receive(IORequest& outReq) = 0;
    virtual void send(const IOResponse& resp) = 0;
    
protected:
    // Platform-specific: read command frame from hardware
    virtual bool readCommandFrame(cmdFrame_t& frame) = 0;
    
    // Shared helpers
    IORequest convertToIORequest(const cmdFrame_t& frame);
    bool commandNeedsData(std::uint8_t command) const;
    
    Channel& _channel;
    const BusTraits& _traits;
    State _state{State::WaitingForCommand};
    std::vector<std::uint8_t> _rxBuffer;
    RequestID _nextRequestId{1};
};

} // namespace fujinet::io::transport::legacy
```

**ByteBasedLegacyTransport** (for SIO, IEC):
```cpp
class ByteBasedLegacyTransport : public LegacyTransport {
public:
    bool receive(IORequest& outReq) override;  // Implements ACK/NAK flow
    void send(const IOResponse& resp) override; // Implements COMPLETE/ERROR + data
    
protected:
    // Protocol-specific control bytes
    virtual void sendAck() = 0;
    virtual void sendNak() = 0;
    virtual void sendComplete() = 0;
    virtual void sendError() = 0;
    
    // Protocol-specific data frames
    virtual std::size_t readDataFrame(std::uint8_t* buf, std::size_t len) = 0;
    virtual void writeDataFrame(const std::uint8_t* buf, std::size_t len) = 0;
};
```

**PacketBasedLegacyTransport** (for IWM):
```cpp
class PacketBasedLegacyTransport : public LegacyTransport {
public:
    bool receive(IORequest& outReq) override;  // Implements packet parsing
    void send(const IOResponse& resp) override; // Implements status + data packets
    
protected:
    // Protocol-specific packet methods
    virtual void sendStatusPacket(std::uint8_t status_byte) = 0;
    virtual void sendDataPacket(const std::uint8_t* buf, std::size_t len) = 0;
};
```

### 6. Platform-Specific Transport Implementation

**SIO Transport** (inherits from ByteBasedLegacyTransport):
```cpp
namespace fujinet::io::transport::legacy {

class SioTransport : public ByteBasedLegacyTransport {
public:
    explicit SioTransport(Channel& channel)
        : ByteBasedLegacyTransport(channel, make_sio_traits())
    {
        _hardware = make_sio_hardware();
    }
    
protected:
    bool readCommandFrame(cmdFrame_t& frame) override {
        // Wait for CMD pin assertion, read 5-byte frame from UART
        // Uses hardware abstraction for GPIO/UART access
    }
    
    void sendAck() override { _hardware->write('A'); }
    void sendNak() override { _hardware->write('N'); }
    void sendComplete() override {
        _hardware->delayMicroseconds(DELAY_T5);
        _hardware->write('C');
    }
    void sendError() override {
        _hardware->delayMicroseconds(DELAY_T5);
        _hardware->write('E');
    }
    
    std::size_t readDataFrame(std::uint8_t* buf, std::size_t len) override {
        // Read data + checksum, validate, send ACK/NAK
    }
    
    void writeDataFrame(const std::uint8_t* buf, std::size_t len) override {
        // Write data + checksum
    }
    
private:
    std::unique_ptr<BusHardware> _hardware;
};

} // namespace fujinet::io::transport::legacy
```

**IWM Transport** (inherits from PacketBasedLegacyTransport):
```cpp
class IwmTransport : public PacketBasedLegacyTransport {
public:
    explicit IwmTransport(Channel& channel)
        : PacketBasedLegacyTransport(channel, make_iwm_traits())
    {
        _hardware = make_iwm_hardware();
    }
    
protected:
    bool readCommandFrame(cmdFrame_t& frame) override {
        // Check phase lines, decode IWM packet to cmdFrame_t
    }
    
    void sendStatusPacket(std::uint8_t status_byte) override {
        // Encode and send IWM status packet via SPI
    }
    
    void sendDataPacket(const std::uint8_t* buf, std::size_t len) override {
        // Encode and send IWM data packet via SPI
    }
    
private:
    std::unique_ptr<BusHardware> _hardware;
};
```

## Conversion Logic

### cmdFrame_t → IORequest

```cpp
IORequest LegacyFrameParser::toIORequest(const cmdFrame_t& frame, RequestID id) {
    IORequest req;
    req.id = id;
    req.deviceId = _traits.map_device_id(frame.device);
    req.type = RequestType::Command; // Legacy protocols are command-based
    req.command = static_cast<uint16_t>(frame.comnd);
    
    // Convert aux1/aux2 to params
    req.params.clear();
    req.params.push_back(static_cast<uint32_t>(frame.aux1));
    req.params.push_back(static_cast<uint32_t>(frame.aux2));
    
    // Payload will be read separately via readDataFrame()
    req.payload.clear();
    
    return req;
}
```

### IOResponse → Legacy Response

The conversion depends on `ResponseStyle`:

**SIO (AckNakThenData):**
```cpp
void SioTransport::send(const IOResponse& resp) {
    if (resp.status == StatusCode::Ok) {
        sendAck();
        // Wait for device to process...
        sendComplete();
        writeDataFrame(resp.payload.data(), resp.payload.size());
    } else {
        sendNak(); // or sendError() depending on error type
    }
}
```

**IWM (StatusThenData):**
```cpp
void IwmTransport::send(const IOResponse& resp) {
    uint8_t status = mapStatusCodeToIwmStatus(resp.status);
    iwm_send_packet(resp.deviceId, status, resp.payload.data(), resp.payload.size());
}
```

## Platform-Specific Implementations

### File Structure

```
include/fujinet/io/transport/legacy/
├── legacy_transport.h          # Base class
├── legacy_frame_parser.h       # Frame parser
└── cmd_frame.h                # Common cmdFrame_t definition

src/lib/transport/legacy/
├── legacy_transport.cpp
└── legacy_frame_parser.cpp

include/fujinet/platform/legacy/
└── bus_traits.h               # BusTraits registry interface

src/platform/posix/legacy/
├── sio_traits.cpp             # SIO traits for POSIX (NetSIO)
└── iec_traits.cpp             # IEC traits for POSIX

src/platform/esp32/legacy/
├── sio_traits.cpp             # SIO traits for ESP32 (UART + GPIO)
├── iwm_traits.cpp             # IWM traits for ESP32
├── iec_traits.cpp             # IEC traits for ESP32
└── adamnet_traits.cpp         # AdamNet traits for ESP32

include/fujinet/io/transport/legacy/
├── sio_transport.h            # SIO transport
├── iwm_transport.h            # IWM transport
├── iec_transport.h            # IEC transport
└── adamnet_transport.h        # AdamNet transport

src/lib/transport/legacy/
├── sio_transport.cpp          # Platform-agnostic SIO logic
├── iwm_transport.cpp          # Platform-agnostic IWM logic
├── iec_transport.cpp          # Platform-agnostic IEC logic
└── adamnet_transport.cpp      # Platform-agnostic AdamNet logic
```

### Platform-Specific Hardware Access

For hardware-specific operations (GPIO, UART, etc.), use platform registries:

```cpp
namespace fujinet::platform::legacy {

class BusHardware {
public:
    virtual ~BusHardware() = default;
    
    // GPIO operations
    virtual bool commandAsserted() = 0;
    virtual bool motorAsserted() = 0;
    virtual void setInterrupt(bool level) = 0;
    
    // UART/Serial operations
    virtual size_t read(uint8_t* buf, size_t len) = 0;
    virtual void write(const uint8_t* buf, size_t len) = 0;
    virtual void flush() = 0;
    virtual size_t available() = 0;
    
    // Timing
    virtual void delayMicroseconds(uint32_t us) = 0;
};

// Platform factories
std::unique_ptr<BusHardware> make_sio_hardware();
std::unique_ptr<BusHardware> make_iwm_hardware();
std::unique_ptr<BusHardware> make_iec_hardware();

} // namespace fujinet::platform::legacy
```

## Device ID Mapping

Legacy protocols use different device ID schemes. Map them to internal `DeviceID`:

```cpp
// In bus_traits.h
DeviceID BusTraits::map_device_id(uint8_t wire_id) const {
    // Platform-specific mapping
    // Example for SIO:
    if (wire_id == 0x70) return to_device_id(WireDeviceId::FujiNet);
    if (wire_id >= 0x31 && wire_id <= 0x3F) return wire_id; // Disk devices
    if (wire_id >= 0x71 && wire_id <= 0x78) return wire_id; // Network devices
    // ...
    return 0; // Invalid
}
```

## Example: Complete SIO Transport Flow

```cpp
// 1. Setup (in main_esp32.cpp or main_posix.cpp)
auto sioTraits = platform::legacy::make_sio_traits();
auto sioHardware = platform::legacy::make_sio_hardware();
auto sioChannel = platform::create_channel_for_sio(*sioHardware);
auto sioTransport = std::make_unique<SioTransport>(*sioChannel, sioTraits);
core.ioService().addTransport(sioTransport.get());

// 2. Poll loop (in LegacyTransport::poll())
void LegacyTransport::poll() {
    // Read raw bytes from channel
    uint8_t temp[256];
    while (_channel.available()) {
        size_t n = _channel.read(temp, sizeof(temp));
        _rxBuffer.insert(_rxBuffer.end(), temp, temp + n);
    }
}

// 3. Receive (in LegacyTransport::receive())
bool LegacyTransport::receive(IORequest& outReq) {
    if (_state != State::WaitingForCommand) {
        return false; // Still processing previous command
    }
    
    cmdFrame_t frame;
    if (!readCommandFrame(frame)) {
        return false; // No complete frame yet
    }
    
    if (!_traits.validate_checksum(frame)) {
        sendNak();
        return false;
    }
    
    sendAck();
    
    // Convert to IORequest
    outReq = _parser.toIORequest(frame, _nextRequestId++);
    
    // If command requires data, read it
    if (needsDataFrame(outReq.command)) {
        _state = State::WaitingForData;
        // Data will be read in next receive() call
    }
    
    return true;
}

// 4. Send (in LegacyTransport::send())
void LegacyTransport::send(const IOResponse& resp) {
    switch (_traits.response_style) {
        case ResponseStyle::AckNakThenData:
            if (resp.status == StatusCode::Ok) {
                sendComplete();
                writeDataFrame(resp.payload.data(), resp.payload.size());
            } else {
                sendError();
            }
            break;
        // ... other response styles
    }
    
    _state = State::WaitingForCommand;
}
```

## Testing Strategy

1. **Unit tests**: Test `LegacyFrameParser` with mock traits
2. **Integration tests**: Test full transport with mock hardware
3. **Protocol tests**: Use Python test harness to send legacy frames and verify responses

## Migration Path

1. **Phase 1**: Implement `BusTraits` registries for each platform
2. **Phase 2**: Implement `LegacyTransport` base class
3. **Phase 3**: Implement platform-specific transports (SIO, IWM, IEC)
4. **Phase 4**: Add to main initialization
5. **Phase 5**: Test with real hardware

## Protocol Comparison

| Feature | SIO (Byte-Based) | IWM (Packet-Based) |
|---------|------------------|-------------------|
| Control bytes | ACK/NAK/COMPLETE/ERROR | None |
| Response style | Control byte + data | Status packet + data packet |
| Checksum | Additive wrap-around | XOR |
| Hardware | UART + GPIO | SPI + GPIO |
| Device IDs | Fixed (0x70, 0x31-0x3F, etc.) | Dynamic (assigned during INIT) |
| Flow control | Time-based (T4, T5 delays) | Phase-based (idle/reset/enable) |

## Benefits

1. **Clean separation**: Platform-specific code isolated in registries
2. **No duplication**: Common logic in base class, traits capture differences
3. **Type safety**: Protocol differences enforced by inheritance hierarchy
4. **No no-op methods**: Each protocol only implements what it uses
5. **Testable**: Mock traits and hardware for unit tests
6. **Extensible**: New protocols inherit from appropriate base class
7. **Maintainable**: Single codebase for all platforms, no `#ifdef` soup
