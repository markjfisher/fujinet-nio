# **FujiNet-NIO Architecture Document**

*Version 1.1 — Updated 2025-12-05*

---

## **Table of Contents**
1. [Overview](#overview)  
2. [Design Principles](#design-principles)  
3. [High-Level Layered Architecture](#high-level-layered-architecture)  
4. [Data Flow: Host → Core → Device → Host](#data-flow-host--core--device--host)  
5. [Core Components](#core-components)  
   - [Channel](#1-channel-lowest-layer)  
   - [Transport](#2-transport-framing--protocol-layer)  
   - [FujiBus / SLIP Protocol](#3-fujibus--slip-protocol-layer)  
   - [IODeviceManager](#4-iodevicemanager-device-routing-and-lifecycle)  
   - [VirtualDevice](#5-virtualdevice-abstract-device-api)  
   - [IOService](#6-ioservice-transport-polling--routing)  
   - [FujinetCore](#7-fujinetcore-overall-engine)  
6. [Build Profiles, Channels & Transports](#build-profiles-channels--transports)  
7. [Hardware Capabilities](#hardware-capabilities)  
8. [Platform Implementations](#platform-implementations)  
9. [UML Diagrams](#uml-diagrams)  
10. [Testing Strategy](#testing-strategy)  
11. [Future Enhancements](#future-enhancements)

---

# **Overview**

FujiNet-NIO is a modern, cross-platform re-implementation of the FujiNet firmware I/O architecture.

It is designed to:

- Run on **ESP32-S3 (ESP-IDF, TinyUSB)**  
- Run on **POSIX systems** (Linux/macOS)  
- Be embeddable as a **native library**  
- Integrate with **emulators**  
- Eventually compile to **WebAssembly**

At the heart of the design is a clean separation between:

| Layer | Purpose |
|-------|---------|
| **Channels** | Raw byte I/O (USB CDC, PTY, TCP, UART, …) |
| **Transports** | Framing (SLIP), FujiBus encode/decode → IORequest/IOResponse |
| **Core** | Request routing, ticking, device lifecycle |
| **Devices** | Virtual devices (Disk, Fuji config, Network, Printer, …) |

This replaces the previous macro-heavy, platform-entangled architecture with a testable, layered core.

---

# **Design Principles**

### ✔ 1. No `#ifdef` spaghetti

Platform differences are isolated behind:

- **Build Profiles** (`fujinet::build`)
- **Channel factories** (`fujinet::platform::*`)
- **Platform-specific hardware capability detection**

Core types (transports, devices, IOService, routing) are **`#ifdef`-free**.

---

### ✔ 2. Explicit, testable boundaries

Each layer has a narrow, focused API:

- **Channel** → raw bytes (`read` / `write`)  
- **Transport** → `IORequest` / `IOResponse`  
- **VirtualDevice** → business logic (`handle` / `poll`)  

This makes mocking, fuzzing, and unit testing straightforward.

---

### ✔ 3. Protocol correctness

FujiBus and SLIP are treated as first-class protocol layers:

- Their parsing/encoding is isolated
- They have their own tests
- They can be reused with different Channels (e.g., PTY, USB, TCP)

---

### ✔ 4. Zero shared state between layers

- Devices don’t know about transports or channels  
- Transports don’t know about device internals  
- Channels don’t know about FujiBus or SLIP  

Everything flows through the **core** via `IORequest` and `IOResponse`.

---

### ✔ 5. Cross-platform reproducibility

The same device logic and protocol parsing runs on:

- ESP32-S3 (with TinyUSB + LittleFS)
- POSIX (PTY + normal filesystem)
- Future Pi/other boards

Only a thin platform layer differs.

---

# **High-Level Layered Architecture**

```text
 ┌──────────────────────────────────────────────┐
 │                 Host System                  │
 │  (Atari, C64, Emulator, Test Script, Pi)     │
 └──────────────────────────────────────────────┘
                      │
                      ▼
           Channel (raw byte I/O)
    USB CDC, PTY, TCP socket, UART, emulator pipe
                      │
                      ▼
      Transport (Framing + FujiBus protocol)
     - SLIP framing / unframing
     - FujiBus header parsing
     - Build IORequest / encode IOResponse
                      │
                      ▼
                FujinetCore
      - Routing and device management
      - Tick loop
                      │
                      ▼
           VirtualDevice subsystem
  Disk, Fuji config, Network, Printer, CP/M, etc.
                      │
                      ▼
      Transport encodes IOResponse → Channel.write()
```

---

# **Data Flow: Host → Core → Device → Host**

1. **Transport reads bytes from Channel**  
   - Calls `channel.read()` when `channel.available()` is true  
   - Appends to an internal `_rxBuffer`

2. **SLIP frame extracted**  
   - Uses SLIP `END` delimiters to find frame boundaries  
   - Validates escape sequences

3. **FujiBus parsed**  
   - Parses header: device, command, length, checksum  
   - Parses descriptors → parameter list (`params`)  
   - Remaining bytes → `payload`  

   Produces an `IORequest`:

   ```cpp
   struct IORequest {
       RequestID                id;
       DeviceID                 deviceId;
       RequestType              type;     // usually Command for FujiBus
       std::uint16_t            command;
       std::vector<uint32_t>    params;
       std::vector<uint8_t>     payload;
   };
   ```

4. **IOService routes request**  
   - `IOService::serviceOnce()` pulls requests from transports  
   - For each request: `IODeviceManager::handleRequest()`  

5. **VirtualDevice handles request**  
   - Device-specific logic (Disk, Fuji config, etc.)  
   - Returns an `IOResponse`:

   ```cpp
   struct IOResponse {
       RequestID                id;
       DeviceID                 deviceId;
       StatusCode               status;
       std::uint16_t            command;
       std::vector<uint8_t>     payload;
   };
   ```

6. **Transport encodes IOResponse**  
   - Constructs FujiBus packet from `IOResponse`  
   - SLIP-encodes it  
   - Calls `channel.write()` to send to host

---

# **Core Components**

---

## **1. Channel (lowest layer)**

### Purpose

Abstracts **raw byte I/O**:

- No knowledge of SLIP
- No knowledge of FujiBus
- No device / protocol logic

### API

```cpp
class Channel {
public:
    virtual bool available() = 0;
    virtual std::size_t read(std::uint8_t* buffer, std::size_t maxLen) = 0;
    virtual void write(const std::uint8_t* buffer, std::size_t len) = 0;
    virtual ~Channel() = default;
};
```

### Implementations

| Platform | Channel implementation |
|----------|------------------------|
| POSIX    | `PtyChannel` (in `platform/posix/channel_factory.cpp`) |
| ESP32-S3 | `UsbCdcChannel` (in `platform/esp32/usb_cdc_channel.cpp`) |
| Future   | TCP channels, UART-based SIO, WebUSB, emulator pipes |

Channels are platform-specific and discovered via **channel factories**.  
Core code only uses the `Channel` interface.

---

## **2. Transport (framing + protocol layer)**

The primary transport today is **FujiBus over SLIP**:

```cpp
class FujiBusTransport : public ITransport {
public:
    explicit FujiBusTransport(Channel& ch);

    bool poll() override;
    bool receive(IORequest& out) override;
    void send(const IOResponse& resp) override;
};
```

### Responsibilities

- Maintain an internal receive buffer  
- Read raw bytes from the `Channel`  
- Extract SLIP frames  
- Parse FujiBus packet into `IORequest`  
- Encode `IOResponse` into FujiBus + SLIP  
- Pass requests/responses to/from `IOService`

Transports are **stateless with respect to devices**—they only speak protocol.

---

## **3. FujiBus & SLIP Protocol Layer**

This layer provides low-level primitive operations:

- SLIP byte definitions and encode/decode
- FujiBus header, descriptors, payload
- Checksum computation

Typical responsibilities:

- `decodeSlip(buffer) → vector<uint8_t>`  
- `encodeSlip(data) → bytes`  
- `parseFujiBus(bytes) → FujiBusPacket`  
- `serializeFujiBus(packet) → bytes`

The transport (`FujiBusTransport`) uses these helper functions to:

- Convert raw bytes from channel → `IORequest`  
- Convert `IOResponse` → raw bytes to channel

This separation allows:

- Direct unit tests of protocol logic
- Reuse of FujiBus on other channels (TCP, WebSocket, etc.)

---

## **4. IODeviceManager (device routing and lifecycle)**

### Responsibilities

- Map `DeviceID` → `VirtualDevice*`  
- Forward `IORequest` to the correct device  
- Poll all devices each tick for background work

### API

```cpp
class IODeviceManager {
public:
    bool registerDevice(DeviceID id, std::unique_ptr<VirtualDevice> dev);
    bool unregisterDevice(DeviceID id);

    VirtualDevice* getDevice(DeviceID id);

    IOResponse handleRequest(const IORequest& request);
    void pollDevices();
};
```

If a `DeviceID` is not registered, `handleRequest()` returns an `IOResponse` with `StatusCode::DeviceNotFound`.

---

## **5. VirtualDevice (abstract device API)**

Devices represent logical endpoints for the host:

- Fuji core/config device
- Disk / Host slot device
- Network device
- Printer, CP/M, modem, etc.

### API

```cpp
class VirtualDevice {
public:
    virtual ~VirtualDevice() = default;

    virtual IOResponse handle(const IORequest& request) = 0;

    // Optional periodic work (e.g. time-based events)
    virtual void poll() {}
};
```

Each device is **fully decoupled** from:

- channels
- transports
- platform specifics

They just receive `IORequest` and return `IOResponse`.

---

## **6. IOService (transport polling + routing)**

`IOService` glues transports and devices together.

### Responsibilities

- Own a set of `ITransport*` (non-owning)  
- Poll each transport for new requests  
- Forward requests to `IODeviceManager`  
- Send responses back via the originating transport

### API (simplified)

```cpp
class IOService {
public:
    explicit IOService(IORequestHandler& handler); // usually IODeviceManager

    void addTransport(ITransport* t);
    void serviceOnce();
};
```

`serviceOnce()` is called once per core tick.

---

## **7. FujinetCore (overall engine)**

`FujinetCore` bundles all the core pieces:

```cpp
class FujinetCore {
public:
    FujinetCore();

    IODeviceManager& deviceManager();
    IOService&       ioService();
    RoutingManager&  routing();

    void addTransport(io::ITransport* t);
    void tick();
    std::uint64_t tick_count() const;

private:
    std::uint64_t   _tickCount{0};
    IODeviceManager _deviceManager;
    RoutingManager  _routing;
    IOService       _ioService;
};
```

### `tick()` lifecycle

```cpp
void FujinetCore::tick()
{
    _ioService.serviceOnce();
    _deviceManager.pollDevices();
    ++_tickCount;
}
```

The platform main loop calls `core.tick()` on a schedule (e.g., every 20–50ms).

---

## Dependency Injection Architecture

FujiNet-NIO uses **explicit constructor-based dependency injection** to maintain clear boundaries between layers.

Unlike many C++ codebases, VirtualDevices do **not**:

- access global singletons  
- reach into `FujinetCore`  
- call platform APIs directly  
- perform hidden cross-layer lookups  

Instead, all dependencies are passed in at creation time by the platform bootstrap code.

### Example: Injecting StorageManager

```cpp
class FujiDevice : public VirtualDevice {
public:
    FujiDevice(ResetHandler reset,
               std::unique_ptr<FujiConfigStore> configStore,
               fs::StorageManager& storage)
        : _reset(std::move(reset))
        , _configStore(std::move(configStore))
        , _storage(storage)
    {}
};
```

Wiring occurs at the platform level (ESP32 or POSIX):

```cpp
auto config = create_fuji_config_store(core.storageManager());
auto device = std::make_unique<FujiDevice>(
    reset_handler,
    std::move(config),
    core.storageManager()
);

core.deviceManager().registerDevice(FujiDeviceId::FujiNet,
                                    std::move(device));
```

### Why We Do This

1. **Encapsulation**  
   Each VirtualDevice knows only what it needs; nothing more.

2. **Testability**  
   Devices can be tested in isolation using mock `StorageManager`, mock network clients, etc.

3. **Platform independence**  
   ESP32-specific or POSIX-specific behavior never leaks into device code.

4. **Predictable architecture**  
   The platform layer becomes the *composition root*, similar to DI containers in frameworks such as Micronaut or NestJS.

### What Devices May Depend On

A VirtualDevice may receive:

- `StorageManager&`
- `RoutingManager&`
- `FujiConfigStore`
- network or filesystem facades
- any other well-defined, platform-agnostic interfaces

### What Devices Must Not Depend On

- Platform APIs (ESP-IDF, POSIX syscalls)
- Global variables
- Full `FujinetCore` access
- Transports, channels, or other low-level components

These boundaries ensure that FujiNet-NIO remains portable, maintainable, and scalable as we add more platforms, devices, and protocols.

---

# **Build Profiles, Channels & Transports**

## BuildProfile

File: `include/fujinet/build/profile.h`

```cpp
namespace fujinet::build {

enum class Machine {
    Generic,
    FujiNetESP32,
    Atari8Bit,
    C64,
    Apple2,
    FujiNetPi,
};

enum class TransportKind {
    FujiBus,
    SIO,
    IEC,
};

enum class ChannelKind {
    Pty,
    UsbCdcDevice,
    TcpSocket,
};

struct HardwareCapabilities; // see later

struct BuildProfile {
    Machine              machine;
    TransportKind        primaryTransport;
    ChannelKind          primaryChannel;
    std::string_view     name;
    HardwareCapabilities hw;
};

BuildProfile current_build_profile();

} // namespace fujinet::build
```

## Mapping build flags to BuildProfile

File: `src/lib/build_profile.cpp`

```cpp
BuildProfile current_build_profile()
{
#if defined(FN_BUILD_ATARI)
    BuildProfile p{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::Pty, // placeholder
        .name             = "Atari + SIO",
    };

#elif defined(FN_BUILD_ESP32_USB_CDC)
    BuildProfile p{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .name             = "ESP32-S3 + FujiBus over USB CDC",
    };

#else
    BuildProfile p{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "POSIX + FujiBus over PTY",
    };
#endif

    p.hw = detect_hardware_capabilities();
    return p;
}
```

**This file is the only place in the codebase that reads `FN_BUILD_*` macros.**

---

# **Hardware Capabilities**

To avoid hard-coding ESP-specific concepts (Wi-Fi, PSRAM, etc.) into core code, we use a semantic capabilities struct.

```cpp
struct NetworkCapabilities {
    bool hasLocalNetwork        {false}; // Can we open TCP/UDP at all?
    bool managesItsOwnLink      {false}; // ESP32 Wi-Fi config vs host OS
    bool supportsAccessPointMode{false}; // SoftAP for configuration portal
};

struct MemoryCapabilities {
    std::size_t persistentStorageBytes{0}; // usable storage for FS/config
    std::size_t largeMemoryPoolBytes  {0}; // big RAM pool (e.g., PSRAM)
    bool hasDedicatedLargePool        {false};
};

struct UsbCapabilities {
    bool hasUsbDevice{false};
    bool hasUsbHost  {false};
};

struct HardwareCapabilities {
    NetworkCapabilities network;
    MemoryCapabilities  memory;
    UsbCapabilities     usb;
};
```

### Platform responsibilities

- **ESP32** implementation reads:
  - flash size, PSRAM size
  - Wi-Fi / AP support
  - USB device/host support
- **POSIX / Pi** implementation:
  - marks network available (`hasLocalNetwork = true`)
  - does not manage its own Wi-Fi
  - treats storage/RAM as effectively large

Core logic then decides:

- whether to register a **Wi-Fi config device** (needs `managesItsOwnLink`)  
- whether to enable **network devices** (needs `hasLocalNetwork`)  
- how big to make **caches** (based on `largeMemoryPoolBytes`)

---

# **Platform Implementations**

## POSIX

- Entry point: `src/app/main_posix.cpp`
- Uses:
  - `build::current_build_profile()`
  - `platform::create_channel_for_profile(profile)`
  - `core::setup_transports(core, *channel, profile)`
- Implements:
  - `PtyChannel` in `platform/posix/channel_factory.cpp`
  - POSIX `logging.cpp`
  - POSIX `hardware_caps.cpp`
  - POSIX config store factory (YAML on normal filesystem)

Loop:

```cpp
int main() {
    FujinetCore core;
    auto profile = build::current_build_profile();
    auto channel = platform::create_channel_for_profile(profile);

    core::setup_transports(core, *channel, profile);

    while (true) {
        core.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
```

---

## ESP32-S3 (ESP-IDF)

- Entry point: `src/app/main_esp32.cpp`
- Uses:
  - TinyUSB-based `UsbCdcChannel`
  - LittleFS on internal flash
  - ESP32 logging adapter (`platform/esp32/logging.cpp`)
  - ESP32 hardware capability detection

Typical startup:

```cpp
extern "C" void app_main(void)
{
    // Init logging, LittleFS, etc.
    platform::esp32::init_littlefs();

    core::FujinetCore core;
    auto profile = build::current_build_profile();

    auto channel = platform::create_channel_for_profile(profile);
    core::setup_transports(core, *channel, profile);

    // Register devices, including FujiDevice (config, reset, etc.)

    for (;;) {
        core.tick();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
```

---

# **UML Diagrams**

We maintain PlantUML diagrams in `docs/uml/`:

- `core_io.puml` — core I/O classes and relationships  
- `arch_core_io.puml` — layered architecture between Channel, Transport, Core, Devices  
- `arch_platform_channels.puml` — how platform-specific channels plug into core  
- `arch_protocol_fujibus.puml` — FujiBus/SLIP protocol diagram  

Generated SVGs live in `docs/images/` and are referenced by docs.

Example:

![Core + IO auto-gen UML](images/core_io.svg)

---

# **Testing Strategy**

## Unit Tests (host / POSIX)

- SLIP encode/decode
- FujiBus packet parsing
- IODeviceManager routing and error cases
- Individual VirtualDevices (e.g., FujiDevice config parsing)

## Integration Tests (POSIX)

- PTY-based tests using Python scripts (`tools/fuji_send.py`)
- End-to-end FujiBus → IORequest → VirtualDevice → IOResponse → bytes

## ESP32 Hardware Tests

- USB CDC loopback and stability tests
- Sending real FujiBus commands from host to FujiDevice and verifying behaviour
- FS tests (LittleFS mount, config persistence)

## Future: Emulator / WASM Tests

- In-process tests using a socket/pipe-backed Channel  
- Running identical device logic inside an emulator process

---

# **Future Enhancements**

1. **Additional transports**
   - Full SIO and IEC transports
   - TCP-based transport for emulators and testing

2. **Expanded device set**
   - CP/M device
   - Rich Network device (TNFS, HTTP, HTTPS)
   - Printer and modem devices

3. **Advanced routing**
   - Multiple transports active simultaneously
   - Dynamic routing rules per device

4. **OTA and configuration UX**
   - Web-based config UI backed by YAML/JSON store
   - Configurable per-profile defaults

5. **WebAssembly target**
   - Channels implemented in JS (WebSockets / WebUSB)
   - Same FujiBus + VirtualDevices running in-browser

---

This document is the **authoritative** description of FujiNet-NIO architecture.  
If core concepts change (BuildProfile, Channel/Transport split, HardwareCapabilities), update this file alongside the code and UML diagrams.
