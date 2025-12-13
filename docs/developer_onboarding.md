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

This can be generated as follows:

```
❯ tree -a -I '.pio|build|.git|managed_components|docs|third_party|.git*|*.lock|.vscode|sdkconfig.*'
.
├── boards
│   └── esp32-s3-wroom-1-n16r8.json
├── build.sh
├── clang-uml.yml
├── CMakeLists.txt
├── CMakePresets.json
├── fujinet_posix.cmake
├── include
│   └── fujinet
│       ├── config
│       │   ├── fuji_config.h
│       │   └── fuji_config_yaml_store_fs.h
│       ├── core
│       │   ├── bootstrap.h
│       │   ├── core.h
│       │   ├── file_device_init.h
│       │   └── logging.h
│       ├── fs
│       │   ├── filesystem.h
│       │   ├── fs_stdio.h
│       │   └── storage_manager.h
│       ├── io
│       │   ├── core
│       │   │   ├── channel.h
│       │   │   ├── io_device_manager.h
│       │   │   ├── io_message.h
│       │   │   ├── request_handler.h
│       │   │   └── routing_manager.h
│       │   ├── devices
│       │   │   ├── file_codec.h
│       │   │   ├── file_commands.h
│       │   │   ├── file_device.h
│       │   │   ├── fuji_commands.h
│       │   │   ├── fuji_device.h
│       │   │   └── virtual_device.h
│       │   ├── protocol
│       │   │   ├── fuji_bus_packet.h
│       │   │   └── wire_device_ids.h
│       │   └── transport
│       │       ├── fujibus_transport.h
│       │       ├── io_service.h
│       │       └── transport.h
│       └── platform
│           ├── channel_factory.h
│           ├── esp32
│           │   ├── fs_factory.h
│           │   ├── fs_init.h
│           │   ├── pinmap.h
│           │   └── usb_cdc_channel.h
│           ├── fuji_config_store_factory.h
│           ├── fuji_device_factory.h
│           └── posix
│               └── fs_factory.h
├── LICENSE
├── pio-build
│   ├── ini
│   │   ├── platformio.common.ini
│   │   ├── platformio.zip-options.ini
│   │   └── platforms
│   │       ├── platformio-cdc-fujibus-s3-wroom-1-n16r8.ini
│   │       ├── platformio-sio-legacy-s3-wroom-1-n16r8.ini
│   │       └── README.md
│   ├── partitions
│   │   └── partitions_16MB.csv
│   ├── scripts
│   │   ├── create-platformio-ini.py
│   │   └── create-sdkconfig.py
│   └── sdkconfig
│       ├── platform_sdkconfig_map.txt
│       ├── sdkconfig-common.defaults
│       ├── sdkconfig-fs-littlefs.defaults
│       ├── sdkconfig-optimizations-to-review.defaults
│       ├── sdkconfig-spiram-oct80.defaults
│       └── sdkconfig-tinyusb.defaults
├── platformio.ini
├── platformio.local.ini
├── py
│   └── fujinet_tools
│       ├── cli.py
│       ├── fileproto.py
│       ├── fujibus.py
│       └── __init__.py
├── pyproject.toml
├── README.md
├── scripts
│   ├── build_pio.sh
│   ├── build_posix.sh
│   ├── fujinet
│   ├── gen_uml.sh
│   └── update_cmake_sources.py
├── src
│   ├── app
│   │   ├── main_esp32.cpp
│   │   └── main_posix.cpp
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── lib
│   │   ├── bootstrap.cpp
│   │   ├── build_profile.cpp
│   │   ├── file_device.cpp
│   │   ├── file_device_init.cpp
│   │   ├── fs_stdio.cpp
│   │   ├── fuji_bus_packet.cpp
│   │   ├── fujibus_transport.cpp
│   │   ├── fuji_config_yaml_store.cpp
│   │   ├── fuji_device.cpp
│   │   ├── fujinet_core.cpp
│   │   ├── fujinet_init.cpp
│   │   ├── io_device_manager.cpp
│   │   ├── io_service.cpp
│   │   ├── routing_manager.cpp
│   │   └── storage_manager.cpp
│   └── platform
│       ├── esp32
│       │   ├── channel_factory.cpp
│       │   ├── fs_factory.cpp
│       │   ├── fs_init.cpp
│       │   ├── fuji_config_store_factory.cpp
│       │   ├── fuji_device_factory.cpp
│       │   ├── hardware_caps.cpp
│       │   ├── logging.cpp
│       │   ├── pinmap.cpp
│       │   └── usb_cdc_channel.cpp
│       └── posix
│           ├── channel_factory.cpp
│           ├── fs_factory.cpp
│           ├── fuji_config_store_factory.cpp
│           ├── fuji_device_factory.cpp
│           ├── hardware_caps.cpp
│           └── logging.cpp
└── tests
    ├── CMakeLists.txt
    ├── doctest.h
    ├── run_main.cpp
    ├── test_embed_core.cpp
    ├── test_fujipacket.cpp
    └── test_smoke.cpp
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
- `FujiBusTransport`

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

## Dependency Injection (How Devices Access Core Services)

FujiNet-NIO does **not** use global singletons or service locators.  
Instead, it follows a simple and explicit *dependency injection* model:

### Design Rules

- **VirtualDevices never fetch global state.**  
  They should not reach into `FujinetCore` or platform APIs directly.

- **All dependencies are passed through constructors.**  
  If a device needs something (e.g., `StorageManager`, `FujiConfigStore`, network clients), the platform/bootstrap code injects it:

  ```cpp
  auto device = std::make_unique<FujiDevice>(
      reset_handler,
      std::move(config_store),
      core.storageManager()
  );
  core.deviceManager().registerDevice(WireDeviceId::FujiNet, std::move(device));
  ```

- **The platform layer is the “composition root.”**  
  It wires together devices, transports, channels, and configuration.

- **Each device explicitly declares what it needs.**  
  This makes devices unit-testable on POSIX and reduces coupling.

### Why This Matters

- Devices become reusable and testable.
- The core library stays clean and platform-agnostic.
- ESP32 vs POSIX differences never leak into device logic.
- No global state ⇒ predictable, debuggable behaviour.

This pattern mirrors dependency injection approaches from Micronaut or NestJS, but implemented manually and explicitly in C++.

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
scripts/fujinet TODO PARAMS
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
scripts/fujinet TODO \
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

