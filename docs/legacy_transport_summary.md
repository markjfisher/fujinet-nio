# Legacy Transport Implementation Summary

## Quick Start

This document provides a concise summary of how to implement legacy bus protocol support in fujinet-nio. See `legacy_transport_design.md` for detailed architecture.

## Key Design Decisions

### 1. Transport Pattern (Follows Existing FujiBusTransport)

Each legacy bus becomes an `ITransport` implementation:

```cpp
class SioTransport : public ITransport {
    bool receive(IORequest& outReq) override;
    void send(const IOResponse& resp) override;
};
```

This integrates seamlessly with existing `IOService` - no changes needed to core.

### 2. Platform Traits Registry (No Ifdefs)

Platform-specific behaviors captured in registries:

```cpp
// Platform-agnostic interface
namespace fujinet::platform::legacy {
    BusTraits make_sio_traits();
    BusTraits make_iwm_traits();
}

// Platform-specific implementations
// src/platform/esp32/legacy/sio_traits.cpp
BusTraits make_sio_traits() {
    BusTraits traits;
    traits.checksum = sio_checksum;  // Function pointer
    traits.ack_delay = 250;           // microseconds
    traits.response_style = ResponseStyle::AckNakThenData;
    return traits;
}
```

### 3. Hardware Abstraction (Platform Factories)

Hardware access via platform factories:

```cpp
// Platform-agnostic interface
namespace fujinet::platform::legacy {
    std::unique_ptr<BusHardware> make_sio_hardware();
}

// Platform-specific: src/platform/esp32/legacy/sio_hardware.cpp
std::unique_ptr<BusHardware> make_sio_hardware() {
    return std::make_unique<SioHardwareEsp32>();
}
```

## Implementation Checklist

### Step 1: Define Bus Traits Interface

**File**: `include/fujinet/platform/legacy/bus_traits.h`

```cpp
namespace fujinet::platform::legacy {

struct BusTraits {
    using ChecksumFn = std::function<uint8_t(const uint8_t*, size_t)>;
    ChecksumFn checksum;
    
    uint32_t ack_delay;
    uint32_t complete_delay;
    
    enum class ResponseStyle { AckNakThenData, StatusThenData, ImmediateData };
    ResponseStyle response_style;
    
    DeviceID map_device_id(uint8_t wire_id) const;
};

BusTraits make_sio_traits();
BusTraits make_iwm_traits();
// ... other platforms

} // namespace
```

### Step 2: Implement Platform Traits

**File**: `src/platform/esp32/legacy/sio_traits.cpp`

```cpp
namespace fujinet::platform::legacy {

BusTraits make_sio_traits() {
    BusTraits traits;
    traits.checksum = [](const uint8_t* buf, size_t len) {
        // SIO checksum algorithm
        uint8_t chk = 0;
        for (size_t i = 0; i < len; i++) {
            chk = ((chk + buf[i]) >> 8) + ((chk + buf[i]) & 0xff);
        }
        return chk;
    };
    traits.ack_delay = 250;
    traits.complete_delay = 250;
    traits.response_style = BusTraits::ResponseStyle::AckNakThenData;
    traits.map_device_id = [](uint8_t wire_id) -> DeviceID {
        // Map SIO device IDs to internal DeviceID
        if (wire_id == 0x70) return to_device_id(WireDeviceId::FujiNet);
        // ... other mappings
        return wire_id; // Default: pass through
    };
    return traits;
}

} // namespace
```

### Step 3: Create Legacy Transport Base

**File**: `include/fujinet/io/transport/legacy/legacy_transport.h`

```cpp
namespace fujinet::io::transport::legacy {

class LegacyTransport : public ITransport {
public:
    explicit LegacyTransport(
        Channel& channel,
        const platform::legacy::BusTraits& traits
    );
    
    void poll() override;
    bool receive(IORequest& outReq) override;
    void send(const IOResponse& resp) override;
    
protected:
    // Platform-specific overrides
    virtual bool readCommandFrame(cmdFrame_t& frame) = 0;
    virtual void sendAck() = 0;
    virtual void sendNak() = 0;
    virtual void sendComplete() = 0;
    virtual void sendError() = 0;
    virtual size_t readDataFrame(uint8_t* buf, size_t len) = 0;
    virtual void writeDataFrame(const uint8_t* buf, size_t len) = 0;
    
private:
    Channel& _channel;
    const platform::legacy::BusTraits& _traits;
    std::vector<uint8_t> _rxBuffer;
    RequestID _nextRequestId{1};
};

} // namespace
```

### Step 4: Implement Platform-Specific Transport

**File**: `include/fujinet/io/transport/legacy/sio_transport.h`

```cpp
namespace fujinet::io::transport::legacy {

class SioTransport : public LegacyTransport {
public:
    explicit SioTransport(Channel& channel);
    
protected:
    bool readCommandFrame(cmdFrame_t& frame) override;
    void sendAck() override;
    void sendNak() override;
    void sendComplete() override;
    void sendError() override;
    size_t readDataFrame(uint8_t* buf, size_t len) override;
    void writeDataFrame(const uint8_t* buf, size_t len) override;
    
private:
    std::unique_ptr<platform::legacy::BusHardware> _hardware;
};

} // namespace
```

**File**: `src/lib/transport/legacy/sio_transport.cpp`

```cpp
namespace fujinet::io::transport::legacy {

SioTransport::SioTransport(Channel& channel)
    : LegacyTransport(channel, platform::legacy::make_sio_traits())
    , _hardware(platform::legacy::make_sio_hardware())
{
}

bool SioTransport::readCommandFrame(cmdFrame_t& frame) {
    // Wait for CMD pin assertion (platform-specific)
    if (!_hardware->commandAsserted()) {
        return false;
    }
    
    // Read 5-byte command frame
    if (_hardware->read((uint8_t*)&frame, sizeof(frame)) != sizeof(frame)) {
        return false;
    }
    
    // Validate checksum using traits
    uint8_t calc = _traits.checksum((uint8_t*)&frame.commanddata, 4);
    return calc == frame.checksum;
}

void SioTransport::sendAck() {
    _hardware->write('A');
    _hardware->delayMicroseconds(_traits.ack_delay);
}

// ... other methods

} // namespace
```

### Step 5: Convert cmdFrame_t → IORequest

**File**: `src/lib/transport/legacy/legacy_transport.cpp`

```cpp
IORequest LegacyTransport::convertToIORequest(const cmdFrame_t& frame) {
    IORequest req;
    req.id = _nextRequestId++;
    req.deviceId = _traits.map_device_id(frame.device);
    req.type = RequestType::Command;
    req.command = static_cast<uint16_t>(frame.comnd);
    
    // Convert aux1/aux2 to params
    req.params = {
        static_cast<uint32_t>(frame.aux1),
        static_cast<uint32_t>(frame.aux2)
    };
    
    return req;
}
```

### Step 6: Convert IOResponse → Legacy Format

```cpp
void LegacyTransport::send(const IOResponse& resp) {
    switch (_traits.response_style) {
        case BusTraits::ResponseStyle::AckNakThenData:
            if (resp.status == StatusCode::Ok) {
                sendComplete();
                writeDataFrame(resp.payload.data(), resp.payload.size());
            } else {
                sendError();
            }
            break;
        // ... other styles
    }
}
```

### Step 7: Register Transport in Main

**File**: `src/app/main_esp32.cpp` (or `main_posix.cpp`)

```cpp
// After creating core and devices...

// Create legacy SIO transport
auto sioChannel = platform::create_channel_for_sio();
if (sioChannel) {
    auto sioTransport = std::make_unique<transport::legacy::SioTransport>(*sioChannel);
    core.ioService().addTransport(sioTransport.get());
    // Keep alive (store in vector or similar)
    transports.push_back(std::move(sioTransport));
}
```

## Platform-Specific Considerations

### Atari SIO
- **Hardware**: UART + GPIO (CMD pin, INT pin)
- **Checksum**: Additive wrap-around
- **Response**: ACK/NAK → COMPLETE/ERROR → data + checksum
- **Timing**: Critical delays (T4, T5)

### Apple II IWM
- **Hardware**: SPI + GPIO (phase lines, REQ, ACK)
- **Checksum**: Different algorithm
- **Response**: Status packet → data packet
- **Timing**: Phase-based, not time-based

### Commodore IEC
- **Hardware**: GPIO (ATN, CLK, DATA, SRQ)
- **Checksum**: None (IEC protocol handles errors)
- **Response**: IEC bus protocol (ATN sequences)
- **Timing**: IEC bus timing (not delays, but state machine)

## Testing

1. **Unit tests**: Mock `BusTraits` and `BusHardware`
2. **Integration tests**: Use Python test harness with real frames
3. **Hardware tests**: Test with actual hardware

## Benefits

✅ **No `#ifdef`s** in core transport code  
✅ **Platform-agnostic** business logic  
✅ **Clean separation** of concerns  
✅ **Testable** with mocks  
✅ **Extensible** for new platforms  

## Next Steps

1. Review and refine `BusTraits` interface
2. Implement SIO transport first (most mature protocol)
3. Add IWM, IEC, AdamNet incrementally
4. Test with real hardware
5. Document protocol-specific quirks
