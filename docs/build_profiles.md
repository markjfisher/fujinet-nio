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
#if defined(FN_BUILD_ATARI)
    return {
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .primaryChannel   = ChannelKind::Pty,      // until real SIO HW
        .name             = "Atari + SIO",
        .hw               = detect_hardware_capabilities(),
    };

#elif defined(FN_BUILD_ESP32_USB_CDC)
    return {
        .machine          = Machine::FujiNetESP32,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .name             = "ESP32-S3 + FujiBus over USB CDC",
        .hw               = detect_hardware_capabilities(),
    };

#else
    return {
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBus,
        .primaryChannel   = ChannelKind::Pty,
        .name             = "POSIX + FujiBus over PTY",
        .hw               = detect_hardware_capabilities(),
    };
#endif
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
    // future: HardwareSio, Uart0, etc.
}
```

---

# 7. Why this model is correct

### 🟢 Clean separation of concerns

| Concept | Meaning | Examples |
|--------|---------|----------|
| **Transport** | Protocol spoken over the link | FujiBus, SIO |
| **Channel** | How raw bytes move | PTY, USB-CDC, TCP |
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
