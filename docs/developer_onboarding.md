# Developer Onboarding Guide

## **FujiNet-NIO Project**  

*Version 1.1 — 2026-01*

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
- ClockDevice
- ModemDevice

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
### Installing Prequisites

Install prerquisites from packages:

> **_NOTE:_** PlatformIO Core can be installed from packages but they may be old versions.  Using the VSCode extension will install the current version.

- Ubuntu (optionally add `platformio` to install PlatformIO Core)

  ```sh
  apt install --no-install-recommends python3 pipx cmake build-essential # platformio
  ```

- Arch (optionally add `platformio-core` to install PlatformIO Core)

  ```sh
  pacman -S python python-uv cmake base-devel # platformio-core
  ```

Build:
```
# Show build script overview, including build boards and profiles
./build.sh -h

# List available POSIX build profiles (included in main help, but here on their own for scripting)
./build.sh -p -S

# Build a POSIX target (-c will clean the build dir first)
./build.sh -c -p fujibus-pty-debug

# Args can be combined:
./build.sh -cp fujibut-pty-debug
```

Run the posix build via the runner script if you want reboot behaviour to restart the application automatically:
```
# cd to the build target
cd build/fujibus-pty-debug
# This is useful if you have a terminal in the build folder that gets wiped by building in another terminal. "cd ." will fix the folder having been removed from under you.
cd . && ./run-fujinet-nio

```

If you build the POSIX app using the PTY channel, you will see something similar to:

```
[PtyChannel] Created PTY. Connect to: /dev/pts/7
```

Note the pts channel that is opened.

You can send FujiBus packets with the CLI tool:
```
# Example: mount a disk image
./scripts/fujinet -p /dev/pts/7 disk mount --slot 1 --fs host --path /path/to/image.ssd --type auto

# Show CLI help
./scripts/fujinet -p /dev/pts/7 --help
```

---

## 4.2 ESP32-S3 (ESP-IDF via PlatformIO)

Install dependencies:
- PlatformIO (VSCode extension recommended)
- ESP32-S3 toolchain (auto-installed by PIO)

Build:
```
# Show ESP32 build help (includes available boards)
./build.sh -h

# Clean and build ESP32 (default build type)
./build.sh -cb

# Or explicitly specify ESP32 build type with -e
./build.sh -e -cb
```

Flash:
```
./build.sh -u
```

Monitor:
```
./build.sh -m
```

Setup new board:
```
# List available boards
./build.sh -S

# Setup a new board (creates platformio.local.ini)
./build.sh -s BOARD_NAME
```

On ESP32-S3, communication is handled through **TinyUSB CDC-ACM**:
- CDC0 = debug logging          e.g. /dev/ttyACM0
- CDC1 = FujiBus data channel        /dev/ttyACM1

---

# 5. Console interaction

The diagnostics framework is described in (diagnostis.md)[diagnostics.md]

If you build the application with a console (on posix this is automatic, on esp32 you must build a board with it enabled on a particular channel, e.g. uart or cdc)
then you can interact with the running fujinet via the console.

## Example posix session

```shell
# Build and run the posix instance in one terminal:
❯ ./build.sh -cp fujibus-pty-debug
❯ cd build/fujibus-pty-debug

❯ cd . && ./run-fujinet-nio
Starting fujinet-nio
[I] nio: fujinet-nio starting (POSIX app)
[I] nio: Version: 0.1.1
[Console] PTY created. Connect diagnostic console to: /dev/pts/5
...

# In a new terminal, use picocom or similar to connect to the console device:
❯ picocom -q /dev/pts/5
fujinet-nio diagnostic console (type: help)
>
```

## Example esp32 session

```shell
# Build and run the esp32 instance, with monitoring
# Assumption, you are using the `fujibus-usbcdc-consolecdc-s3-wroom-1-n16r8` board where the console is CDC.
# You can also use UART with "consoleuart" board, and commands are then in the pio MONITOR, or via uart port of the esp32
❯ ./build.ch -cbu
❯ picocom -q /dev/fujinet-console    # or use /dev/ttyACM2 if you haven't setup udev rules. The exact port can be tricky, hence why udev rules are much nicer
>
```

For infomation on setting up /dev/fujinet-console as a symlink to the CDC port when you plug in the 2nd usb cable to the S3, see
the (diagnostis.md)[diagnostics.md] documentation for setting up udev rules.
As mentioned above, you can directly use `/dev/ttyACM<N>` if you know exactly which port is correct, usually ttyACM2.

## Running commands in the console

You can now interact with the console, start by typing 'help' to see the available commands.

The console is split into 2 parts:
- Interacting with the file system using standard ls/rm/mv/cd type commands after specifying which "file system" to use with "fs"
- Interacting with the diagnostics modules, e.g. "core.info"

```
> help
commands:
  cd - change directory; use fs:/ to select filesystem
  fs - list mounted filesystems
  help - show this help
  kill - terminate fujinet-nio (stops the process)
  ls - list directory (or stat file)
  mkdir - create directory
  mv - rename within a filesystem
  pwd - show current filesystem path
  reboot - reboot/reset via platform hook (if supported)
  rm - remove file(s)
  rmdir - remove directory

diagnostics:

  [core]
    core.info - build/version information
    core.stats - core runtime statistics

  [disk]
    disk.slots - list disk slots (mounted images, geometry, state)

  [modem]
    modem.at - send an AT command and return the modem's response
    modem.baud - set modem baud (informational; affects CONNECT messaging)
    modem.baudlock - enable/disable baud lock
    modem.drain - drain pending modem output bytes (if any)
    modem.status - show modem state (mode, listen, baud, cursors)

  [net]
    net.close - close a session handle (or all)
    net.sessions - list active network sessions/handles
```

An example output from the diagnostics information is disk.slots, listing mounted slots:
```
> disk.slots
status: ok
slot=1 inserted=1 ro=0 dirty=1 changed=1 type=ssd ss=256 sc=400 last_err=none image=host:/test.ssd
slot=2 inserted=0 ro=0 dirty=0 changed=0 type=auto ss=0 sc=0 last_err=none
slot=3 inserted=0 ro=0 dirty=0 changed=0 type=auto ss=0 sc=0 last_err=none
slot=4 inserted=0 ro=0 dirty=0 changed=0 type=auto ss=0 sc=0 last_err=none
slot=5 inserted=0 ro=0 dirty=0 changed=0 type=auto ss=0 sc=0 last_err=none
slot=6 inserted=0 ro=0 dirty=0 changed=0 type=auto ss=0 sc=0 last_err=none
slot=7 inserted=0 ro=0 dirty=0 changed=0 type=auto ss=0 sc=0 last_err=none
slot=8 inserted=0 ro=0 dirty=0 changed=0 type=auto ss=0 sc=0 last_err=none
```

## Exiting the console

You need to issue keystrokes that quit the client, e.g. in picocom `ctrl-a ctrl-x`.

---

# 5. Adding a New Virtual Device

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

# 6. Adding a New Transport

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

# 7. Adding a New Channel

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

# 8. Coding Standards

- **C++20**  
- `std::unique_ptr` for ownership  
- No raw `new/delete`  
- Avoid `#ifdef` outside platform or profile factories  
- All platform differences isolated in `/src/platform`  
- All protocol logic lives in `/src/lib` and `/include/fujinet/io`  
- No business logic in app entry points  
- All devices must be unit-testable  

---

# 9. Debugging Tips

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

## 10. CLI commands for testing (HTTP & TCP)

During development it is often useful to manually interact with a running
**fujinet-nio** instance from the command line. This allows rapid validation
of protocol behavior, debugging of edge cases, and experimentation without
writing new firmware or tests.

The project provides Python-based CLI tooling under:

    py/fujinet_tools/

These tools communicate with the device over the FujiBus transport (USB/CDC,
PTY, etc.) and exercise the NetworkDevice using the same binary protocol as
production hosts.

### Supported testing workflows

The CLI tooling supports:

- HTTP/HTTPS testing
  - GET / POST / PUT / HEAD
  - Request body streaming
  - Response chunking
  - Header and status inspection
- TCP stream testing
  - Interactive REPL
  - Sequential read/write cursor validation
  - Non-blocking `NotReady` / `DeviceBusy` behavior
  - TCP pseudo headers via `Info()`

### Local test services (Docker)

For convenience, the repository includes scripts to start local test servers:

    [Start Test Services Script](scripts/start_test_services.sh)

This script can start:
- an HTTP test server (httpbin)
- a TCP echo server (via netshoot + socat)
- or both together

These services provide stable, repeatable endpoints for validating both HTTP
and TCP protocol implementations.

### Detailed guide

A full, step-by-step guide covering:
- starting the test services
- HTTP testing from the CLI
- TCP testing using the interactive REPL
- common testing patterns and troubleshooting

is provided in:

    [CLI device testing](docs/cli_device_testing.md)

Developers are encouraged to use these tools regularly when working on:
- NetworkDevice protocol changes
- HTTP/TCP backend implementations
- Transport-layer fixes (USB/CDC, PTY, etc.)

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

