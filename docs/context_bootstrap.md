# FUJINET-NIO PROJECT CONTEXT BOOTSTRAP

---

## OVERVIEW

We are building **fujinet-nio**, a modern cross-platform rewrite of FujiNet’s I/O subsystem.  
Targets:
- POSIX (Linux/macOS)
- ESP32-S3 (ESP-IDF + TinyUSB)
- Future embedded platforms
- WebAssembly

Goals:
- Clean layered architecture
- No `#ifdef` spaghetti
- Unified FujiBus+SLIP wire protocol
- Modern C++ memory-safe patterns
- Composable transports and channels
- Strong testing foundation

---

## ARCHITECTURE LAYERS

### 1. Channels (lowest layer)
Namespace: `fujinet::io`

Interface:
```
class Channel {
public:
    virtual bool available() = 0;
    virtual std::size_t read(uint8_t*, std::size_t) = 0;
    virtual void write(const uint8_t*, std::size_t) = 0;
    virtual ~Channel() = default;
};
```

Implementations:
- `PtyChannel` (POSIX PTY)
- `UsbCdcChannel` (ESP32-S3 TinyUSB CDC)
- Additional hardware channels later

Responsibility: **raw byte I/O, no framing**.

---

### 2. Transports
Interface: `fujinet::io::ITransport`

Current implementation:  
`FujiBusTransport`

Responsibilities:
- Accumulate bytes from Channel
- Extract **SLIP** frame  
- Parse **FujiBus packet header, descriptors, parameters**
- Produce `IORequest`
- Send `IOResponse` (SLIP + FujiBus encoding)

Transport → Core → Device → Transport loop works.

---

### 3. FujiBus Protocol
We removed legacy enums and standardized naming:

```
enum class FujiDeviceId : uint8_t { ... };
enum class FujiCommandId : uint8_t { ... };
enum class SlipByte : uint8_t { End=0xC0, Escape=0xDB, EscEnd=0xDC, EscEsc=0xDD };
constexpr uint8_t to_byte(SlipByte b);
```

FujiBus header is 6 bytes:
- device
- command
- length
- checksum
- descriptor

Python script (`tools/fuji_send.py`) can:
- Build SLIP+FujiBus packet
- Send it
- Read response
- Decode & pretty-print header/params/payload

---

### 4. IORequest / IOResponse (unified core protocol)

```
struct IORequest {
    RequestID id;
    DeviceID deviceId;
    RequestType type;
    uint8_t command;
    std::vector<uint8_t> payload;
};

struct IOResponse {
    RequestID id;
    DeviceID deviceId;
    StatusCode status;
    uint16_t command;      // echo of request.command
    std::vector<uint8_t> payload;
};
```

---

### 5. IODeviceManager

Responsibilities:
- Register/unregister devices
- Route IORequests based on DeviceID
- Call `VirtualDevice::handle()`
- `pollDevices()` for background tasks

```
unordered_map<DeviceID, unique_ptr<VirtualDevice>> _devices;
```

---

### 6. IOService

Responsibilities:
- Holds **non-owned** transports
- On each tick:
  - poll transports
  - retrieve IORequests
  - route via IODeviceManager
  - send IOResponses back

```
void addTransport(ITransport*);
void serviceOnce();
```

---

### 7. FujinetCore

```
class FujinetCore {
    IODeviceManager _deviceManager;
    RoutingManager  _routing;
    IOService       _ioService;
    uint64_t        _tickCount;
};
```

tick():
- ioService.serviceOnce()
- deviceManager.pollDevices()
- tickCount++

---

## CURRENT STATUS

### Working:
- USB CDC TinyUSB channel on ESP32-S3
- POSIX PTY channel
- SLIP decode/encode
- FujiBusPacket (C++ + Python)
- End-to-end I/O flow from host → device → host
- DummyDevice functioning end-to-end
- Python tool can send/receive Fuji packets

### In progress:
- Creating real **FujiDevice** implementation
- Mapping Fuji commands → device logic
- Strengthening protocol parsing
- Expanding developer documentation & UML diagrams

---

## NEXT STEPS (WHERE WE LEFT OFF)

1. Improve `fuji_send.py` output (decode header/fields neatly)
2. Implement the first real Fuji virtual device
3. Replace DummyDevice in ESP32 app
4. Expand test suite
5. Continue improving architecture diagrams

---

# END OF CONTEXT BOOTSTRAP
Paste everything above into a new session.
