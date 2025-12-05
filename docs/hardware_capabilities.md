# FujiNet-NIO Hardware Capabilities Model

## Overview

The original FujiNet firmware tightly coupled hardware properties (flash, PSRAM, Wi-Fi, etc.) directly into device logic.  
FujiNet-NIO separates these concerns:

- **BuildProfile** describes *intended* platform (ESP32, POSIX, Pi, Atari+SIO, etc.).
- **HardwareCapabilities** describes *actual runtime hardware features*.
- Platform-specific code populates these capabilities; core logic remains clean and portable.

This allows:

- multi-platform builds (ESP32, Pi, Linux)
- correct configuration of devices at runtime
- conditional enabling of features (e.g., Wi-Fi setup AP, large caches)

---

## Capability Groups

### 1. NetworkCapabilities

| Field | Meaning |
|-------|---------|
| `hasLocalNetwork` | Can the system open TCP/UDP sockets at all? |
| `managesItsOwnLink` | Does FujiNet itself manage Wi-Fi / Ethernet link configuration? (ESP32=yes, POSIX=no) |
| `supportsAccessPointMode` | Can this platform run a Wi-Fi AP for configuration? |

### ESP32
- Has built-in TCP/IP stack  
- Firmware controls Wi-Fi  
- Supports SoftAP  

### POSIX / Raspberry Pi
- Uses OS networking stack  
- FujiNet does *not* configure Wi-Fi  
- No SoftAP from FujiNet process  

---

### 2. MemoryCapabilities

| Field | Meaning |
|-------|---------|
| `persistentStorageBytes` | Size of nonvolatile storage available for FujiNet (LittleFS, FATFS, SD, or unlimited on POSIX) |
| `largeMemoryPoolBytes` | Size of large RAM pool (PSRAM on ESP32-S3; “effectively unlimited” on POSIX) |
| `hasDedicatedLargePool` | Indicates PSRAM or similar memory distinct from main RAM |

### Why this abstraction?

It avoids hard-coding hardware specifics (flash, PSRAM) and instead expresses meaning:

- How much storage for config/mounts?
- How much RAM for caches or big files?
- Is memory constrained?

This generalises cleanly across:

- ESP32-S3 (flash + PSRAM)
- ESP32-C3 (flash only)
- Raspberry Pi (disk + huge RAM)
- Linux/macOS hosts

---

### 3. USB Capabilities

| Field | Meaning |
|-------|---------|
| `hasUsbDevice` | Can the software act as a USB device (e.g., CDC ACM)? |
| `hasUsbHost` | Can it act as USB host (ESP32-S3 supports it; POSIX host mode = OS responsibility) |

---

## BuildProfile Structure

```cpp
struct BuildProfile {
    Machine          machine;
    TransportKind    primaryTransport;
    ChannelKind      primaryChannel;
    std::string_view name;
    HardwareCapabilities hw;
};
```

`current_build_profile()` determines:

- machine identity
- transport protocol (FujiBus, SIO, IEC)
- channel (PTY, USB-CDC, TCP, etc.)

`detect_hardware_capabilities()` (platform implementation) fills:

- runtime memory sizes
- networking capabilities
- USB capabilities

---

## Platform Implementations

### ESP32

Uses:

- `esp_flash_get_size()`
- `esp_psram_get_size()`
- `esp_chip_info()`

Populates:

- real flash size
- PSRAM size
- Wi-Fi/AP support
- USB device/host support (S3 only)

### POSIX / Raspberry Pi

Populates:

- infinite-like storage
- infinite-like RAM
- networking enabled
- does not manage its own Wi-Fi link
- no USB device mode

---

## How Core Uses Capabilities

### Enable devices conditionally

```cpp
if (profile.hw.network.hasLocalNetwork) {
    manager.registerDevice<NetworkDevice>();
}
```

### Only allow Wi-Fi config on ESP32

```cpp
if (profile.hw.network.managesItsOwnLink) {
    manager.registerDevice<WifiConfigDevice>();
}
```

### Memory-aware tuning

```cpp
if (profile.hw.memory.largeMemoryPoolBytes > 4*1024*1024) {
    cache.enableLargeMode();
} else {
    cache.enableSmallMode();
}
```

### Diagnostics

Devices may report:

- platform type
- available FS size
- available PSRAM
- network link type

---

## Benefits

- **Zero `#ifdef` inside core library**  
  Only platform-specific files touch ESP-IDF or POSIX APIs.

- **Future-proof for new architectures**  
  Pi USB gadget mode? ESP32-C6? Easy — just update the platform file.

- **Clear separation of “build identity” vs “runtime capability”**

---
