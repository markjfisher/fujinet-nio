# Developer Onboarding Guide

## **FujiNet-NIO Project**  

*Version 1.0 — 2025-12*

Welcome to the **FujiNet-NIO** project!  
This document provides everything a new developer needs to understand the codebase, set up a development environment, and begin contributing confidently.

---

# 1. Project Overview

FujiNet-NIO is a complete redesign of the FujiNet I/O firmware.  
It is:

- **Cross-platform**: ESP32-S3 (ESP-IDF), POSIX, Emulators, WASM.
- **Protocol-driven**: FujiBus + SLIP framing are first-class protocol layers.
- **Architecturally clean**: No `#ifdef` spaghetti, strong interface boundaries.
- **Testable**: Deterministic, unit-testable business logic.
- **Extensible**: Add new transports, virtual devices, and host protocols easily.

At runtime, FujiNet-NIO acts as a translation layer between:

```
Host Machine (Atari/C64/etc.)
       ↓
  Channel (USB/UART/PTY)
       ↓
 Transport (FujiBus)
       ↓
    Core
       ↓
 Virtual Devices (Disk, Fuji, Network...)
```

---

# 2. Repository Structure

```
/docs                → Architecture, onboarding, diagrams
/include/fujinet     → Public headers
/src/app             → POSIX + ESP32 entry points
/src/lib             → Core engine, IO service, transports, routing
/src/platform        → Channel factories + platform-specific channels
/tests               → Unit tests (POSIX)
/tools               → Python scripts (e.g., fuji_send.py)
platformio.ini       → ESP32 build config
CMakeLists.txt       → POSIX build config
```

---

# 3. Architectural Quickstart

FujiNet-NIO is composed of well-defined layers:

### **Channels**
Raw byte I/O over a medium. Examples:
- `UsbCdcChannel` (ESP32 TinyUSB)
- `PtyChannel` (POSIX)
- Future: UARTChannel, WebUSBChannel, EmulatorChannel

Channels know **nothing** about protocols.

---

### **Transports**
Implement the FujiBus wire protocol.

Responsibilities:
- buffer incoming bytes
- detect complete SLIP frames
- parse FujiBus headers + descriptors
- produce `IORequest`
- encode `IOResponse`

Current implementation:
- `Rs232Transport` (soon renamed → `FujiBusTransport`)

---

### **Core**
The central engine coordinating all I/O:

- `IODeviceManager` → owns all VirtualDevices  
- `RoutingManager` → handles overrides (future extension)  
- `IOService` → polls transports, routes requests  
- `FujinetCore` → top-level orchestrator

---

### **Virtual Devices**
Business logic layer. Each device implements:

```
IOResponse handle(const IORequest&)
void poll()
```

Device examples:
- FujiDevice  
- DiskDevice  
- NetworkDevice  
- PrinterDevice  
- DBCDevice  

---

# 4. Build Setup

## 4.1 POSIX (Linux/macOS)
Dependencies:
```
cmake >= 3.20
gcc/g++ (C++20)
python3
```

Build:
```
mkdir build
cd build
cmake ..
make -j
```

Run:
```
./fujinet-nio
```

The POSIX app uses a **PTY channel**, so you will see:

```
[PtyChannel] Created PTY. Connect to: /dev/pts/7
```

You can send FujiBus packets with:
```
python tools/fuji_send.py --port /dev/pts/7 ...
```

---

## 4.2 ESP32-S3 (ESP-IDF via PlatformIO)

Install dependencies:
- PlatformIO (VSCode extension recommended)
- ESP32-S3 toolchain (auto-installed by PIO)

Build:
```
pio run -e esp32s3-espidf
```

Flash:
```
pio run -e esp32s3-espidf -t upload
```

Monitor:
```
pio device monitor
```

On ESP32-S3, communication is handled through **TinyUSB CDC-ACM**:
- CDC0 = debug logging  
- CDC1 = FujiBus data channel  

---

# 5. Running End-to-End Tests

Use the provided Python script:

```
python tools/fuji_send.py \
    --port /dev/ttyACM1 \
    --device 1 \
    --command 1 \
    --payload "hello world" \
    --read
```

Expected output:

```
Sending:
C0 01 01 ... C0
Received:
C0 01 00 ... C0
```

---

# 6. Adding a New Virtual Device

1. Create a header in `/include/fujinet/io/devices/<device>.h`
2. Implement the class in `/src/lib/devices/<device>.cpp`

Example:

```
class ClockDevice : public VirtualDevice {
public:
    IOResponse handle(const IORequest& req) override {
        if (req.command == 0x01 /* GET TIME */) {
            return {...};
        }
        return {req.id, req.deviceId, StatusCode::Unsupported, {}};
    }

    void poll() override {
        // optional background work
    }
};
```

3. Register inside `main_posix.cpp` or bootstrap logic:

```
core.deviceManager().registerDevice(DEVICE_CLOCK, std::make_unique<ClockDevice>());
```

That’s all: transport → core → device routing is automatic.

---

# 7. Adding a New Transport

1. Implement the `ITransport` interface:

```
bool poll()
bool receive(IORequest&)
void send(const IOResponse&)
```

2. Bind it to a Channel  
3. Add it via:

```
core.addTransport(&myTransport);
```

Use cases:
- SIO transport for Atari  
- IEC for C64  
- Emulator IPC transport  
- WebUSB or WebSocket transport  

---

# 8. Adding a New Channel

Channels represent **byte pipes**, not protocols.

Steps:

1. Implement:

```
bool available()
std::size_t read(...)
void write(...)
```

2. Add to:

```
src/platform/<platform>/channel_factory.cpp
```

3. Bind via BuildProfiles.

---

# 9. Coding Standards

- **C++20**  
- `std::unique_ptr` for ownership  
- No raw `new/delete`  
- Avoid `#ifdef` outside platform or profile factories  
- All platform differences isolated in `/src/platform`  
- All protocol logic lives in `/src/lib` and `/include/fujinet/io`  
- No business logic in app entry points  
- All devices must be unit-testable  

---

# 10. Debugging Tips

### ESP32-S3
- Use CDC0 for logs  
- Use CDC1 for FujiBus  
- Set `ESP_LOG_LEVEL_VERBOSE` when needed  
- Run `idf.py menuconfig → TinyUSB` if logs disappear  

### POSIX
- Use `strace` on the PTY  
- Use the python script to send decoded FujiBus frames  
- Dump SLIP frames using `xxd`, `hexdump`, or Wireshark extcap  

---

# 11. Contributing Workflow

1. Fork repo  
2. Create a feature branch  
3. Write tests if possible  
4. Submit PR with:
   - Description  
   - Architecture impact  
   - API changes  
   - Test coverage  

Merges require:
- Code review  
- CI passing  

---

# 12. Where to Start

If you're new and want actionable first tasks:

### Starter Tasks
- Add a trivial VirtualDevice (EchoDevice)
- Improve fuji_send.py to decode FujiBus headers
- Add SLIP unit tests
- Add a new StatusCode
- Write an emulator loopback test

### Intermediate Tasks
- Implement FujiDevice (core configuration)
- Add support for multi-transport IOService
- Introduce structured logging for devices

### Advanced Tasks
- Add WebUSB transport (via Emscripten)
- Implement Atari SIO or C64 IEC transport
- Improve RoutingManager to support override modes

---

# 13. Additional Documentation

See:

- `docs/architecture.md` — Complete architecture specification  
- `docs/build_profiles.md` — Build configuration system  
- `docs/uml/*.puml` — PlantUML diagrams  

---

# 14. Welcome Aboard 🎉

FujiNet-NIO is built for longevity and clarity.  
Your contributions will help bring modern, clean architecture to retro platforms everywhere.

If you have questions:
- Ask in GitHub Discussions  
- Open an Issue labelled **question**  

Thanks for joining the project!

