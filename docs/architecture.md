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

> Note:
> This diagram describes **structural dependencies**, not runtime startup order.
> Some behaviors in fujinet-nio (such as starting network-dependent services)
> are driven by events rather than by static initialization order.
>
> See *Event-Driven Service Lifecycle Architecture* for details.


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

### Polling / background work

FujinetCore::tick() polls devices each tick. Devices may use poll() for:
- streaming progress (async backends),
- connection/session timeout reaping,
- housekeeping (autosave, etc).

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

core.deviceManager().registerDevice(WireDeviceId::FujiNet,
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

### Platform services

Some capabilities (e.g. Wi-Fi link bring-up) are platform services rather than IO devices.
They are initialized by platform bootstrap and/or a service manager, then used by devices
(e.g. network backends) without coupling device init time to link bring-up time.

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

## Network Architecture

FujiNet NIO treats networking as a device-facing capability built on top of
platform-specific transport implementations. The goal is to provide a consistent,
stream-oriented interface to host systems while allowing each platform to use the
most appropriate native networking stack.

Related protocol docs:

- [`docs/network_device_protocol.md`](network_device_protocol.md) — NetworkDevice v1 binary protocol (Open/Read/Write/Info/Close)
- [`docs/network_device_tcp.md`](network_device_tcp.md) — TCP scheme mapping for NetworkDevice v1

### NetworkDevice

`NetworkDevice` is a virtual I/O device registered with the core like any other
device (e.g. FileDevice, ClockDevice). From the host’s perspective, it exposes a
handle-based API for network operations such as opening URLs, reading response
data, querying status, and closing connections.

Internally, `NetworkDevice` does not implement networking directly. Instead, it
acts as a coordinator for one or more protocol implementations.

`NetworkDevice` is authoritative for:
- session and handle lifetime
- capacity limits and eviction policy
- protocol-level rules (offset sequencing, body length limits, error mapping)
- integration with the core poll/tick loop

Protocol implementations are responsible only for scheme-specific transport
behavior (HTTP request lifecycle, TCP socket I/O, etc.).


### Protocol-based design

Networking is implemented via protocol handlers that conform to a common
interface (e.g. HTTP, HTTPS, TCP etc).

At runtime, `NetworkDevice` is constructed with a **Protocol Registry**:
a map of protocol names (e.g. `"http"`, `"https"`) to factory functions capable
of creating protocol instances.

This design allows:
- Platform-specific implementations (POSIX vs ESP32)
- Optional protocols (stubbed or omitted if unavailable)
- Clean separation between device semantics and transport mechanics

### Sessions and handles

Each successful network OPEN operation creates a **session** and returns an
opaque 16-bit handle to the host.

A session represents a single logical connection or request lifecycle and owns:
- protocol instance state
- read cursor and EOF tracking
- error status
- any backend resources (sockets, HTTP clients, buffers)

Handles are bounded and reused via generation counters to prevent stale access.
Multiple sessions may be active concurrently, allowing the host to interleave
reads across different network connections.

If a handle becomes invalid (due to Close, eviction, or timeout reaping), all
subsequent operations on that handle (Info, Read, Write, Close) will fail with
`InvalidRequest`.

Handles are intentionally non-idempotent: a Close on an unknown or expired handle
is treated as an error rather than a no-op.

### Session capacity and eviction

NetworkDevice maintains a finite pool of concurrent sessions.

When the session pool is exhausted:
- By default, new Open requests fail with `DeviceBusy`.
- If eviction is enabled via Open flags, NetworkDevice may evict the
  least-recently-used active session.

Eviction immediately invalidates the evicted handle. No further guarantees are
made about in-flight operations on that handle.

### Streaming model

All network reads are **stream-oriented**.

- Data is not required to be buffered in full.
- Backends may deliver data incrementally as it becomes available.
- Reads MAY return `NotReady` when no data is currently available.
- Reads returning `Ok` with zero bytes are valid **only** when EOF has been reached.
- EOF is signaled explicitly once the transfer is complete.

This model allows large responses (e.g. multi-megabyte HTTP bodies) to be handled
without excessive memory usage, especially on constrained platforms like ESP32.

### Platform responsibilities

Platforms provide concrete protocol implementations and supporting services:

- POSIX typically uses synchronous libraries (e.g. libcurl) and can often
  determine content length and headers eagerly.
- ESP32 uses asynchronous, event-driven networking (ESP-IDF), relying on stream
  buffers and TCP backpressure rather than full in-memory buffering.

Platform code is also responsible for initializing underlying network links
(e.g. Wi-Fi), but link bring-up is intentionally decoupled from device creation
to avoid delaying early device responsiveness.

### Polling and lifecycle

Devices participate in the normal core tick cycle via `IODeviceManager::pollDevices()`.
For networking, this primarily ensures that the `NetworkDevice` can manage session
lifecycle (timeouts, leaked handles, cleanup) without requiring extra core threads.

Protocol handlers also expose a `poll()` hook. Some backends may use this to
advance asynchronous operations or perform lightweight bookkeeping. Other backends
(e.g. synchronous POSIX implementations, or ESP-IDF HTTP where transfer progress is
driven by the ESP-IDF task/event system) may implement `poll()` as a no-op.

All network progress is considered poll-driven from the perspective of the
NetworkDevice API.

Even when underlying platforms use synchronous libraries (e.g. POSIX libcurl)
or background tasks (e.g. ESP-IDF HTTP), hosts MUST treat Open, Info, Read, and
Write as potentially asynchronous operations and be prepared to handle
`StatusCode::NotReady` via retry.

The poll hook exists to provide a consistent execution model across platforms
and protocols, even when individual backends do not require explicit polling
to advance I/O.

---

## Disk Architecture

FujiNet NIO treats “disk” as a **reusable core service**:

- **DiskService (core)**: mounts disk image files from a named filesystem (`StorageManager`) and exposes sector-level reads/writes.
- **DiskDevice (VirtualDevice)**: wraps DiskService and exposes a small v1 binary command set for tooling and generic hosts.
- **Machine-specific disk protocols** (Atari SIO disk, BBC DFS/MMFS, etc.) should reuse DiskService and implement only their own bus semantics.

Disk protocol doc:

- [`docs/disk_device_protocol.md`](disk_device_protocol.md) — Disk subsystem overview + DiskDevice v1 binary protocol

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

## Wire IDs vs Internal Device & Command IDs

FujiNet-NIO makes a **strict distinction** between *wire-level identifiers* used by transports and *device-level command semantics* used by virtual devices.

This separation is intentional and foundational to supporting multiple transports (FujiBus, legacy FujiNet wire protocol, future transports) without coupling protocol details to device logic.

---

### 1. Wire Device IDs

**Wire device IDs** are the numeric identifiers that appear on the wire in a transport protocol.

Examples:
- FujiBus device byte
- Legacy FujiNet frame device byte
- Future protocol device selectors

These identifiers are **transport concepts**, not virtual device concepts.

They are defined centrally in:

```
include/fujinet/io/protocol/wire_device_ids.h
```

Example:

```
enum class WireDeviceId : std::uint8_t {
    FujiNet     = 0x70,
    DiskFirst   = 0x31,
    DiskLast    = 0x3F,
    NetworkFirst= 0x71,
    NetworkLast = 0x78,

    // FujiNet-NIO extensions
    DiskService = 0xFC,
    NetworkService = 0xFD,
    FileService = 0xFE,
};
```

Key properties:

- Wire device IDs are **stable and protocol-visible**
- They are **shared across all transports**
- They do **not imply ownership** by any particular `VirtualDevice` class
- They are mapped directly into `IORequest.deviceId`

> The previous name `FujiDeviceId` was retired because it incorrectly implied ownership by the `FujiDevice` virtual device. These IDs belong to the *wire protocol*, not the device implementation.

---

### 2. FujiBusPacket: Wire-Level Only

`FujiBusPacket` represents the **raw FEP-004 protocol frame**, not a semantic command.

As such, it contains only **wire-level fields**:

```
class FujiBusPacket {
private:
    WireDeviceId   _device;    // 1-byte wire device id
    std::uint8_t   _command;   // 1-byte wire command
    std::vector<PacketParam> _params;
    std::optional<ByteBuffer> _data;
};
```

Important constraints:

- `FujiBusPacket` does **not** know about virtual devices
- It does **not** contain device-specific command enums
- It only models what is physically present on the wire

Any interpretation of `_command` happens **after** conversion into an `IORequest`.

---

### 3. IORequest: Transport-Agnostic Core Message

All transports convert their incoming frames into a common core type:

```
struct IORequest {
    RequestID     id;
    DeviceID      deviceId;
    std::uint16_t command;
    std::vector<uint8_t> payload;
};
```

Notes:

- `command` is **16-bit by design**
- Transports may populate only the low 8 bits (FujiBus, legacy)
- Future transports may use the full 16-bit range
- Core routing logic treats `command` as opaque

This allows the core to remain protocol-agnostic.

---

### 4. Device-Specific Command IDs

Each `VirtualDevice` defines **its own command enum**, scoped to its behavior and responsibilities.

Commands are **not shared across devices**.

Examples:

#### FujiDevice commands

```
enum class FujiCommand : std::uint8_t {
    Reset   = 0xFF,
    GetSsid = 0xFE,
};

inline FujiCommand to_fuji_command(std::uint16_t raw)
{
    return static_cast<FujiCommand>(
        static_cast<std::uint8_t>(raw)
    );
}
```

#### FileDevice commands

```
enum class FileCommand : std::uint8_t {
    ListDirectory = 0x01,
    // future: Open, Read, Write, Remove, etc.
};

inline FileCommand to_file_command(std::uint16_t raw)
{
    return static_cast<FileCommand>(
        static_cast<std::uint8_t>(raw)
    );
}
```

Key principles:

- Devices decide how much of the command space they consume
- 8-bit command devices intentionally ignore the high byte
- This enables compatibility with legacy 8-bit protocols
- Devices with future 16-bit command spaces can opt in explicitly

---

### 5. Why Command Masking Happens in the Device

Command truncation (e.g. `raw & 0xFF`) is **not a transport responsibility**.

It is a **device-level decision** that expresses:

> “This device defines its command vocabulary in an 8-bit space.”

Benefits:

- Keeps transports simple and uniform
- Allows legacy and modern transports to target the same devices
- Makes device capabilities explicit and self-contained
- Enables future devices to use wider command spaces without breaking existing ones

---

### 6. Routing and Device Lookup

Routing operates purely on `IORequest.deviceId`:

```
Transport → IORequest → RoutingManager → IODeviceManager → VirtualDevice
```

---

# **Testing Strategy**

## Unit Tests (host / POSIX)

- SLIP encode/decode
- FujiBus packet parsing
- IODeviceManager routing and error cases
- Individual VirtualDevices (e.g., FujiDevice config parsing)

## Integration Tests (POSIX)

- PTY-based tests using Python scripts (invoke via `scripts/fujinet`)
- End-to-end FujiBus → IORequest → VirtualDevice → IOResponse → bytes

## ESP32 Hardware Tests

- USB CDC loopback and stability tests
- Sending real FujiBus commands from host to FujiDevice and verifying behaviour
- FS tests (LittleFS mount, config persistence)

## Future: Emulator / WASM Tests

- In-process tests using a socket/pipe-backed Channel  
- Running identical device logic inside an emulator process

---

# Event-Driven Service Lifecycle

Some functionality in fujinet-nio depends on *when* certain conditions become
true at runtime rather than on static initialization order. Examples include:

- network time synchronization (SNTP)
- embedded web services
- future discovery or telemetry services

These concerns are handled using a small, explicit **event-driven lifecycle
model**, rather than being embedded directly into platform handlers (such as
Wi-Fi callbacks).

In this model:

- Platform code detects conditions (e.g. “network got an IP address”)
- Semantic events are published into core-owned event streams
- Services subscribe to events and manage their own startup and shutdown

This keeps:
- platform code focused on hardware and OS integration
- core logic platform-agnostic
- service startup deterministic and extensible

The full design is documented separately in:

[**Event-Driven Service Lifecycle Architecture**](event_driven_services.md)

That document should be consulted when adding new services that depend on
network availability, time synchronization, or other cross-cutting conditions.


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
