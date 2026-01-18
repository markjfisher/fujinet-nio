# Build Profiles, Transports, Channels, and Avoiding `#ifdef` Hell

This document explains how **fujinet-nio** decides:

- which **machine profile** it targets (ESP32, POSIX, Atari + SIO, Raspberry Pi, etc.)
- which **transport protocol** it uses (FujiBus, SIO, IEC…)
- which **channel** bytes travel over (PTY, USB-CDC, TCP…)
- which **hardware capabilities** apply
- how we avoid `#ifdef` spaghetti in the core

The key architectural rule:

> **All build flags are interpreted exactly once in `build_profile.cpp`.  
> Everywhere else we use clean enums, structs, and platform factories.**

---

# 1. Architectural Layers

```
+---------------------------------------------------+
| Application / Platforms (ESP32, POSIX, Pi)        |
| - main_esp32.cpp                                  |
| - main_posix.cpp                                  |
| - platform/* (channel factories, logging, FS)     |
+--------------------------+------------------------+
|   Build Profile Layer    | Hardware Capabilities  |
| build/profile.cpp        | platform-detected hw   |
+--------------------------+------------------------+
|               Core I/O Logic                      |
| FujinetCore, IODeviceManager, RoutingManager      |
| VirtualDevice, ITransport, FujiBusTransport       |
| Channel (abstract)                                |
+---------------------------------------------------+
```

### Rules

- ❌ Core code never sees `#ifdef`
- ❌ Core code never depends on ESP-IDF or POSIX headers
- ✔ Platform code may use `#ifdef FN_PLATFORM_ESP32` etc.
- ✔ Build profile selects transport + channel types
- ✔ Hardware capabilities enable/disable runtime features

---

# 2. Build Profiles

A build profile is **the compile-time identity** of the firmware/app.

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
    FujiBus,      // SLIP + Fuji headers — the standard protocol
    SIO,          // Atari SIO (future)
    IEC,          // C64 IEC (future)
};

enum class ChannelKind {
    Pty,          // POSIX PTY (dev/testing)
    UsbCdcDevice, // USB CDC device mode (ESP32S3, Pi gadget mode later)
    TcpSocket,    // TCP/IP channel for emulators (future)
    UdpSocket,    // UDP socket (for NetSIO protocol)
    HardwareSio,  // ESP32 GPIO-based SIO (UART + GPIO pins)
};

struct BuildProfile {
    Machine          machine;
    TransportKind    primaryTransport;
    ChannelKind      primaryChannel;
    std::string_view name;

    HardwareCapabilities hw;   // Populated at runtime
};

BuildProfile current_build_profile();

} // namespace fujinet::build
```

---

# 3. Mapping Build Flags → Profile (single source of truth)

File: `src/lib/build_profile.cpp`

```cpp
BuildProfile current_build_profile()
{
    BuildProfile profile{};

#if defined(FN_BUILD_ATARI_SIO)
    profile = BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::HardwareSio,
        .name             = "Atari + SIO via GPIO",
        .hw               = {},
    };
#elif defined(FN_BUILD_ATARI_PTY)
    profile = BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "Atari + SIO over PTY (POSIX)",
        .hw               = {},
    };
#elif defined(FN_BUILD_ATARI_NETSIO)
    profile = BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::UdpSocket,
        .name             = "Atari + SIO over NetSIO (UDP)",
        .hw               = {},
    };
#elif defined(FN_BUILD_ESP32_USB_CDC)
    profile = BuildProfile{
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .name             = "S3 + FujiBus over USB CDC",
        .hw               = {},
    };
#elif defined(FN_BUILD_FUJIBUS_PTY)
    profile = BuildProfile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "POSIX + FujiBus over PTY",
        .hw               = {},
    };
#else
    // Default: POSIX-friendly profile when no explicit build profile macro is provided.
    // This keeps local/test builds working without requiring a preset.
    profile = BuildProfile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "POSIX + FujiBus over PTY (default)",
        .hw               = {},
    };
#endif

    profile.hw = detect_hardware_capabilities();
    return profile;
}
```

📌 **This is the only place where build macros are used.**

Everything else works with enums.

---

# 4. Hardware Capabilities

A **runtime** structure filled by platform code:

```cpp
struct HardwareCapabilities {
    struct {
        bool hasLocalNetwork       = false;
        bool managesItsOwnLink     = false;
        bool supportsAccessPointMode = false;
    } network;

    struct {
        std::size_t persistentStorageBytes = 0;
        std::size_t largeMemoryPoolBytes   = 0;
        bool hasDedicatedLargePool         = false;
    } memory;

    struct {
        bool hasUsbDevice = false;
        bool hasUsbHost   = false;
    } usb;
};
```

## ESP32 example

```cpp
cap.network.hasLocalNetwork       = true;
cap.network.managesItsOwnLink     = true;
cap.network.supportsAccessPointMode = true;

cap.memory.persistentStorageBytes = flash_fs_size();
cap.memory.largeMemoryPoolBytes   = esp_psram_get_size();
cap.memory.hasDedicatedLargePool  = (esp_psram_get_size() > 0);

cap.usb.hasUsbDevice              = true;  // TinyUSB CDC
cap.usb.hasUsbHost                = true;  // S3 supports host mode
```

## POSIX example

```cpp
cap.network.hasLocalNetwork       = true;
cap.network.managesItsOwnLink     = false;
cap.network.supportsAccessPointMode = false;

cap.memory.persistentStorageBytes = std::numeric_limits<size_t>::max();
cap.memory.largeMemoryPoolBytes   = std::numeric_limits<size_t>::max();
cap.memory.hasDedicatedLargePool  = false;

cap.usb.hasUsbDevice              = false;
cap.usb.hasUsbHost                = false;
```

---

# 5. Transports (Protocol Layer)

File: `include/fujinet/core/bootstrap.h`

```cpp
io::ITransport* setup_transports(
    FujinetCore& core,
    io::Channel& channel,
    const build::BuildProfile& profile
);
```

File: `src/lib/bootstrap.cpp`

```cpp
switch (profile.primaryTransport) {
    case TransportKind::FujiBus:
        auto* fb = new io::FujiBusTransport(channel);
        core.addTransport(fb);
        return fb;

    case TransportKind::SIO:
    case TransportKind::IEC:
        // TODO: future transports
        return nullptr;
}
```

✔ Only the transport protocol changes here.  
✔ Channel choice is separate.  
✔ No `#ifdef`.

---

# 6. Channel Factories (Platform Layer)

## POSIX

```cpp
std::unique_ptr<io::Channel>
create_channel_for_profile(const BuildProfile& profile)
{
    switch (profile.primaryChannel) {
        case ChannelKind::Pty:
            return create_pty_channel();
        case ChannelKind::TcpSocket:
            return create_tcp_channel();
        default:
            return nullptr;
    }
}
```

## ESP32

```cpp
switch (profile.primaryChannel) {
    case ChannelKind::UsbCdcDevice:
        return std::make_unique<UsbCdcChannel>();
    case ChannelKind::HardwareSio:
        return std::make_unique<HardwareSioChannel>();  // TODO: implement
    // future: Uart0, etc.
}
```

---

# 7. Why this model is correct

### 🟢 Clean separation of concerns

| Concept | Meaning | Examples |
|--------|---------|----------|
| **Transport** | Protocol spoken over the link | FujiBus, SIO |
| **Channel** | How raw bytes move | PTY, USB-CDC, TCP, HardwareSio |
| **Hardware capabilities** | What the device *can* do | Wi-Fi? PSRAM? USB host? |
| **Build profile** | Which combination this firmware targets | ESP32-USB, POSIX-PTY |

### 🟢 No core code depends on platform specifics  
Everything passes through BuildProfile + Channel factory.

### 🟢 Easy to extend  
Want Pi USB gadget mode?  
Add:

```cpp
ChannelKind::UsbCdcDevice
```

Then implement the channel in `platform/pi/usb_cdc_channel.cpp`.

### 🟢 Multiple transports/channel combos per machine  
E.g. FujiBus over USB, or FujiBus over TCP.

---

# 8. Where `#ifdef` is Allowed

### Allowed
- `build_profile.cpp`
- platform folder (`platform/esp32`, `platform/posix`, etc.)
- compiling different channel implementations

### Forbidden
- transports
- IO logic
- FujiDevice
- config system
- routing logic

---

# 9. Adding a New Machine or Transport

1. Add enum entries to:
   - `Machine`
   - `TransportKind`
   - optionally `ChannelKind`
2. Update `current_build_profile()`
3. Extend `setup_transports()`
4. Add new platform channel(s) if needed
5. Add platform capability detection

✔ No core changes  
✔ No scattering of preprocessor logic

---

# 10. Summary

- BuildProfile = identity of the firmware.
- HardwareCapabilities = what the machine can actually do.
- ChannelKind = physical/OS transport (PTY, USB-CDC, TCP…).
- TransportKind = protocol/framing (FujiBus, SIO).
- All build flags are interpreted once.
- Core stays 100% portable and clean.

## Build Profiles and Compile-Time Flags

FujiNet-NIO uses **compile-time build profiles** to describe *what* kind of
device / transport / channel a build is targeting. These are expressed as
`FN_BUILD_*` macros and are interpreted **only once** in
`src/lib/build_profile.cpp`.

### Available build profile flags

Currently supported profiles:

- `FN_BUILD_ATARI_SIO`  
  - `machine          = Machine::Atari8Bit`  
  - `primaryTransport = TransportKind::SIO`  
  - `primaryChannel   = ChannelKind::HardwareSio`  
  - Used for ESP32 builds with GPIO-based SIO hardware

- `FN_BUILD_ATARI_PTY`  
  - `machine          = Machine::Atari8Bit`  
  - `primaryTransport = TransportKind::SIO`  
  - `primaryChannel   = ChannelKind::Pty`  
  - Used for POSIX builds (testing/development) with SIO over PTY

- `FN_BUILD_ATARI_NETSIO`  
  - `machine          = Machine::Atari8Bit`  
  - `primaryTransport = TransportKind::SIO`  
  - `primaryChannel   = ChannelKind::UdpSocket`  
  - Used for POSIX builds with NetSIO protocol over UDP (connects to netsiohub bridge)
  - Requires NetSIO host/port configuration in `fujinet.yaml` (see `NetSioConfig` section)
  - CMake preset: `atari-netsio-debug` / `atari-netsio-release`

- `FN_BUILD_ESP32_USB_CDC`  
  - `machine          = Machine::FujiNetESP32`  
  - `primaryTransport = TransportKind::FujiBus`  
  - `primaryChannel   = ChannelKind::UsbCdcDevice`  
  - Used by the ESP32-S3 build via PlatformIO.

- `FN_BUILD_FUJIBUS_PTY`  
  - `machine          = Machine::Generic`  
  - `primaryTransport = TransportKind::FujiBus`  
  - `primaryChannel   = ChannelKind::Pty`  
  - POSIX dev / test build with FujiBus over a PTY.

Exactly **one** of these must be defined when building the core. If none is
defined, `build_profile.cpp` triggers a compile-time error via:

```cpp
#error "No build profile selected. Define one of: \
FN_BUILD_ATARI_SIO, FN_BUILD_ATARI_PTY, FN_BUILD_ATARI_NETSIO, FN_BUILD_ESP32_USB_CDC, FN_BUILD_FUJIBUS_PTY."
```

This ensures we never silently “default” to some arbitrary environment.

### FN_DEBUG and logging

`FN_DEBUG` controls whether the `FN_LOG*` macros expand to real logging calls or
compile away to no-ops.

- When `FN_DEBUG` is **defined**, `FN_LOGI/W/E(...)` call into the platform
  logging implementation (`src/platform/*/logging.cpp`).
- When `FN_DEBUG` is **not defined**, all `FN_LOG*` macros are compiled out,
  so there is effectively no runtime cost for logging in release builds.

On the POSIX side, `FN_DEBUG` is usually wired to CMake’s build type:

```cmake
target_compile_definitions(fujinet-nio
    PUBLIC
        FN_PLATFORM_POSIX
        $<$<CONFIG:Debug>:FN_DEBUG>
        $<$<BOOL:${FN_BUILD_ATARI_SIO}>:FN_BUILD_ATARI_SIO>
        $<$<BOOL:${FN_BUILD_ATARI_PTY}>:FN_BUILD_ATARI_PTY>
        $<$<BOOL:${FN_BUILD_ATARI_NETSIO}>:FN_BUILD_ATARI_NETSIO>
        $<$<BOOL:${FN_BUILD_FUJIBUS_PTY}>:FN_BUILD_FUJIBUS_PTY>
)
```

That means:

- `Debug` configuration → `FN_DEBUG` is defined  
- `Release` / `RelWithDebInfo` / `MinSizeRel` → `FN_DEBUG` is not defined

### How these flags are set

#### POSIX (CMake)

For native / POSIX builds, these are regular CMake cache options:

```cmake
option(FN_BUILD_ATARI_SIO         "Build for Atari SIO via GPIO (ESP32)" OFF)
option(FN_BUILD_ATARI_PTY         "Build for Atari SIO over PTY (POSIX)" OFF)
option(FN_BUILD_ATARI_NETSIO      "Build for Atari SIO over NetSIO/UDP (POSIX)" OFF)
option(FN_BUILD_ESP32_USB_CDC     "Build for ESP32 USB CDC profile"      OFF)
option(FN_BUILD_FUJIBUS_PTY       "Build for POSIX FujiBus over PTY"     OFF)
```

You pass them on the CMake configure line:

```bash
# POSIX FujiBus over PTY, Debug build
cmake -B build/posix-fujibus-pty-debug \
      -DCMAKE_BUILD_TYPE=Debug \
      -DFN_BUILD_FUJIBUS_PTY=ON

# Atari SIO via GPIO (ESP32), Release build
cmake -B build/atari-sio-release \
      -DCMAKE_BUILD_TYPE=Release \
      -DFN_BUILD_ATARI_SIO=ON

# Atari SIO over PTY (POSIX), Debug build
cmake -B build/atari-pty-debug \
      -DCMAKE_BUILD_TYPE=Debug \
      -DFN_BUILD_ATARI_PTY=ON

# Atari SIO over NetSIO (POSIX), Debug build
# Requires netsiohub running and NetSIO config in fujinet.yaml
# (or NETSIO_HOST/NETSIO_PORT environment variables as fallback)
cmake -B build/atari-netsio-debug \
      -DCMAKE_BUILD_TYPE=Debug \
      -DFN_BUILD_ATARI_NETSIO=ON

# Or use CMakePresets.json:
cmake --preset atari-netsio-debug
cmake --build --preset atari-netsio-debug-build
```

The `target_compile_definitions(fujinet-nio ...)` call then turns those options
into real `-DFN_BUILD_…` defines for all code that links `fujinet-nio`
(including tests).

#### ESP32 (PlatformIO)

For ESP32 / ESP-IDF builds, the profile is selected via `build_flags` in
`platformio.ini`, for example:

```ini
[env:esp32s3-espidf]
platform  = espressif32@6.10.0
framework = espidf
board     = esp32-s3-devkitc-1
build_type = debug
build_unflags = -std=gnu++20
build_flags =
    -std=gnu++17
    -DFN_PLATFORM_ESP32
    -DFN_PLATFORM_ESP32S3
    -DFN_BUILD_ESP32_USB_CDC
```

PlatformIO injects `-DFN_BUILD_ESP32_USB_CDC` into the ESP-IDF CMake build, so
the same `build_profile.cpp` logic is used on both ESP32 and POSIX.

---

## Embedding & Tests

All of these flags are interpreted in **one place**:
`src/lib/build_profile.cpp`. Everywhere else, code uses the strongly-typed
`BuildProfile` struct.

Tests and embedding examples only need to:

- link against `fujinet-nio`, and
- use `build::current_build_profile()`.

The compile-time flags are handled by CMake / PlatformIO and “flow through” via
`target_compile_definitions`.
