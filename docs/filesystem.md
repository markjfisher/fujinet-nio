# Fujinet-NIO Filesystem Architecture

This document describes the filesystem subsystem within **fujinet-nio**, covering:

- The **abstract filesystem interfaces**
- The **streaming file API**
- How filesystems are *registered* and *queried*
- How the system handles **POSIX filesystems**, **ESP32 Flash**, and **ESP32 SD**
- How core logic remains fully **platform agnostic**
- How ESP-IDF’s VFS makes POSIX-like semantics possible on embedded targets
- How configuration loading integrates with the filesystem layer

The goal is to provide a clean, consistent architecture that supports:
Local flash, SD card, host POSIX, TNFS, SMB, FTP, HTTP, and other filesystem types.

---

## 1. Design Goals

1. **Single abstract API** for all filesystem operations.
2. **Streaming I/O**, not bulk string-based operations.
3. Platform-independent core logic — no ESP32 or POSIX logic inside library code.
4. Multiple simultaneous filesystems (flash, SD, network, etc.).
5. Completely replace the old FNFS hierarchy (which was ESP-heavy and macro-heavy).
6. Clean integration with YAML configuration storage.

---

## 2. Abstract Filesystem Interfaces

The filesystem abstraction lives in  
`include/fujinet/fs/filesystem.h`.

### 2.1 `IFile` – Streaming File Handle

This is a lightweight wrapper over an open file stream:

```cpp
class IFile {
public:
    virtual ~IFile() = default;

    virtual std::size_t read(void* dst, std::size_t maxBytes) = 0;
    virtual std::size_t write(const void* src, std::size_t bytes) = 0;

    virtual bool seek(std::uint64_t offset) = 0;
    virtual std::uint64_t tell() const = 0;
    virtual bool flush() = 0;
};
```

This allows:

- Efficient incremental reads/writes
- Ability to back any file with SD, flash, network, memory, etc.
- Transparent buffering or non-buffering

### 2.2 `IFileSystem` – Abstract Filesystem

```cpp
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    virtual FileSystemKind kind() const = 0;
    virtual std::string name() const = 0;

    virtual bool exists(const std::string& path) = 0;
    virtual bool isDirectory(const std::string& path) = 0;

    virtual bool createDirectory(const std::string& path) = 0;
    virtual bool removeFile(const std::string& path) = 0;
    virtual bool removeDirectory(const std::string& path) = 0;
    virtual bool rename(const std::string& from, const std::string& to) = 0;

    virtual std::unique_ptr<IFile> open(
        const std::string& path,
        const char* mode
    ) = 0;

    virtual bool listDirectory(
        const std::string& path,
        std::vector<FileInfo>& outEntries
    ) = 0;
};
```

The **core concepts**:

- Each filesystem has a **root directory** (flash uses `/fujifs`, SD uses `/sdcard`, POSIX may use a host path).
- All paths are **relative to the filesystem root**.
- The implementation decides how those operations map to actual devices.

---

## 3. POSIX Filesystem Implementation

Located in:

```
src/platform/shared/fs_stdio.cpp   (usable on both ESP32 and POSIX)
```

### 3.1 Why it works on ESP32

ESP-IDF provides a **VFS layer** that maps POSIX-like functions:

| Operation | Maps to ESP32 driver |
|----------|-----------------------|
| `fopen()` | LittleFS / FAT / SDMMC |
| `opendir()` | Same |
| `mkdir()` | Same |
| `unlink()` | Same |

**This means ESP32 exposes a usable subset of POSIX I/O.**  
Therefore we can reuse the same implementation for both native and embedded environments.

### 3.2 Stdio-backed filesystem

`StdioFileSystem` implements `IFileSystem` by:

- Prefixing all paths with the filesystem’s root (`/fujifs`, `/sdcard`, or host path).
- Using `::fopen`, `::fread`, `::fwrite`, `::mkdir`, `::opendir`, etc.
- Returning a `StdioFile` object for open files.

This keeps the implementation small and highly portable.

---

## 4. ESP32 Filesystems

### 4.1 Flash (LittleFS)

ESP-IDF mounts LittleFS under a path such as `/fujifs`:

```cpp
esp_vfs_littlefs_register(&conf);
// After this, fopen("/fujifs/foo.txt") read/writes flash.
```

We then register:

```cpp
auto flash = fs::create_stdio_filesystem("/fujifs", "flash", FileSystemKind::LocalFlash);
core.storageManager().registerFileSystem(std::move(flash));
```

### 4.2 SD Card (SDMMC or SPI SD)

After mounting:

```cpp
esp_vfs_fat_sdmmc_mount("/sdcard", ...);
// Or esp_vfs_littlefs for SD LittleFS variants
```

We register:

```cpp
auto sd = fs::create_stdio_filesystem("/sdcard", "sd0", FileSystemKind::LocalSD);
core.storageManager().registerFileSystem(std::move(sd));
```

### Why this works

Once mounted, **SD and Flash both use ESP-IDF’s VFS**, which speaks POSIX.  
Therefore, the Stdio filesystem implementation works identically across platforms.

---

## 5. StorageManager

Located in:

```
include/fujinet/fs/storage_manager.h
src/lib/storage_manager.cpp
```

The registry is intentionally simple:

```cpp
bool registerFileSystem(std::unique_ptr<IFileSystem> fs);

IFileSystem* get(const std::string& name);
std::vector<std::string> listNames() const;
```

This allows:

- Core code to remain unaware of how many FS exist.
- Platforms to register whatever file systems they need.
- Users to choose FS via name ("flash", "sd0", "host", "tnfs0", etc.)

Example:

```cpp
auto& sm = core.storageManager();
auto* flash = sm.get("flash");
auto* sd    = sm.get("sd0");
```

---

## 6. How Core is Platform-Agnostic

Core code **never checks**:

- Whether SD is inserted,
- Whether LittleFS is mounted,
- How ESP32 drivers work,
- How POSIX works.

Instead the platform layer performs all setup:

```
platform/esp32/
    fs_factory.cpp   → mounts flash & SD
    fuji_device_factory.cpp
    fuji_config_store_factory.cpp

platform/posix/
    fs_factory.cpp   → create host filesystem
```

Core code simply does:

```cpp
auto* fs = core.storageManager().get("flash");
auto file = fs->open("/config.yaml", "rb");
```

No platform #ifdefs, ever.

---

## 7. Configuration Storage (`YamlFujiConfigStoreFs`)

The new config store no longer relies on std::fstream.  
Instead it uses the **filesystem abstraction**, allowing config files to live on:

- SD card
- Flash
- Host filesystem (POSIX mode)

### Load logic (simplified)

```cpp
FujiConfig load() override {
    if (primary && primary->exists(_relPath))
        return loadFrom(primary);

    if (backup && backup->exists(_relPath))
        return loadFrom(backup);

    // No config exists anywhere → create defaults and save
    FujiConfig cfg{};
    if (primary) saveTo(primary, cfg);
    else if (backup) saveTo(backup, cfg);

    return cfg;
}
```

### Save logic

```cpp
void save(const FujiConfig& cfg) override {
    if (!primary && !backup)
        return;

    if (primary) saveTo(primary, cfg);
    if (backup)  saveTo(backup, cfg);
}
```

This reproduces the FujiNet firmware behavior:

- Prefer SD.
- Fall back to Flash.
- Keep Flash updated when SD is present.

---

## 8. Directory Behavior

Because `IFileSystem` only exposes:

```cpp
bool listDirectory(const std::string& path, std::vector<FileInfo>& out);
```

platform code chooses:

- Sorting rules
- Filtering hidden files
- Ordering (lexical, date, etc.)

This keeps the interface *minimal* but extensible.

---

## 9. Summary Diagram

```
+------------------------+        +-----------------------+
|     FujinetCore        |        |   YamlFujiConfigStore |
|  (platform-independent)|        |  (uses IFileSystem)   |
+-----------+------------+        +-----------+-----------+
            |                                 |
            v                                 v
+------------------------+        +-----------------------+
|    StorageManager      | <----> |   Any IFileSystem     |
| registry: "flash",     |        |  (SD, Flash, POSIX,   |
|           "sd0", etc.  |        |   TNFS, SMB, etc.)    |
+-----------+------------+        +-----------------------+
            |
            |
     Platform Layer
   (mounts & registers FS)
            |
            |
+----------------------------+
| ESP32 VFS  or  POSIX libc |
| fopen/opendir/mkdir/...   |
+----------------------------+
```

---

## 10. Key Takeaways

- **One unified filesystem interface**, implemented differently per platform.
- **POSIX-style FS implementation is shared** by both ESP32 and desktop.
- Core logic is fully **decoupled from platform filesystem behavior**.
- ESP-IDF VFS makes the ESP32 behave like a minimal POSIX system — enabling the shared implementation.
- `StorageManager` cleanly manages multiple filesystems.
- Config storage now operates through filesystem abstraction, not std::fstream.

---
