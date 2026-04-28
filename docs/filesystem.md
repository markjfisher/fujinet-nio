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
- Users to choose FS via name ("flash", "sd0", "host", "tnfs", etc.)

Example:

```cpp
auto& sm = core.storageManager();
auto* flash = sm.get("flash");
auto* sd    = sm.get("sd0");
```

`StorageManager` also provides URI-oriented helpers:

```cpp
IFileSystem* getByScheme(const std::string& scheme);
std::pair<IFileSystem*, std::string> resolveUri(const std::string& uri);
```

These are useful when a caller has a full URI (for example `http://...`), while normal `FileDevice` style commands still use explicit filesystem name + path (`<fs> <path>`).

---

## 6. URI Paths And Path Resolver Layer

Path parsing and normalization for console-style filesystem commands is now centralized in a dedicated resolver subsystem:

```
include/fujinet/fs/path_resolvers/
src/lib/path_resolvers/
```

The resolver is intentionally extensible:

- `PathResolver` is the orchestrator (ordered handler registry).
- `IPathHandler` defines the extension point for scheme/path families.
- each protocol/path family provides its own handler classes in separate files.

Current handlers include:

- TNFS URI resolver (`tnfs://`, `tnfs+tcp://`, `tnfstcp://`, `tnfs-tcp://`)
- HTTP URI resolver (`http://`, `https://`)
- TNFS prefixed resolver (`tnfs://...` passed as `tnfs:<endpoint/path>`)
- TNFS relative resolver (joining relative paths while preserving endpoint authority)
- generic `<fs>:` prefixed resolver
- generic relative/absolute resolver

### Why this matters

- Console parsing is no longer hardcoded in `console_fs_shell.cpp`.
- Adding new schemes (for example SMB) is a new handler file + registration, not a large parser edit.
- Behavior is shared and testable through resolver unit tests (`tests/test_path_resolver.cpp`).

### Console integration behavior

`FsShell` now delegates path parsing to `PathResolver` and only performs command semantics.

Examples:

- `cd tnfs://192.168.1.101/` -> resolved to `fs_name=tnfs`, `fs_path=tnfs://192.168.1.101/`
- `cd tnfs+tcp://192.168.1.101/` -> resolved to `fs_name=tnfs` with TCP scheme preserved in `fs_path`
- `cd subdir` while in TNFS endpoint cwd -> endpoint authority is preserved by TNFS-specific relative join logic

---

## 7. How Core is Platform-Agnostic

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

## 8. Configuration Storage (`YamlFujiConfigStoreFs`)

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

## 9. Directory Behavior

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

## 10. Summary Diagram

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

## 11. Key Takeaways

- **One unified filesystem interface**, implemented differently per platform.
- **POSIX-style FS implementation is shared** by both ESP32 and desktop.
- Core logic is fully **decoupled from platform filesystem behavior**.
- ESP-IDF VFS makes the ESP32 behave like a minimal POSIX system — enabling the shared implementation.
- `StorageManager` cleanly manages multiple filesystems.
- Config storage now operates through filesystem abstraction, not std::fstream.
- URI parsing for console paths is centralized in `path_resolvers`, not embedded in shell command code.
- Protocol-specific URI behavior (TNFS today, more later) is implemented in protocol-specific resolver handlers.

---

## 12. Config-Driven Mounts

Fujinet-nio supports **config-driven mounts** that are applied to disk runtime slots at startup, similar to the legacy `mount_all()` behavior in FujiNet firmware.

### Mount Configuration Model

The `MountConfig` struct in `fujinet/config/fuji_config.h` defines a mount entry:

```cpp
struct MountConfig {
    int         slot;     // Slot number (1-8). 0 means unassigned.
    std::string uri;      // URI of the resource (e.g., "sd:/disks/img.atr", "tnfs://server/dir/img.atr")
    std::string mode;    // persisted slot policy/default, e.g. "auto", "ro"
    bool        enabled;  // Whether this mount is active
};
```

**Slot semantics**:
- Slots are numbered 1-8 in config (matching user-facing Atari disk slots)
- Internally converted to 0-7 indices for `DiskService`
- `effective_slot()` method returns 0-based index or -1 if unassigned

### Lazy Mounting

Config-defined mounts use **lazy activation** - the mount is stored but not activated until first disk access (read or write). This is critical for network-based filesystems like TNFS:

- **TNFS servers don't need to be available at startup** - no blocking or delays
- **Network connections are created only when needed** - saves resources
- **Startup time is predictable** regardless of network availability

When a slot has a pending mount:
1. First read/write operation triggers mount activation
2. URI is resolved to filesystem + path
3. Image is opened and becomes available

The pending mount info is stored in `DiskService::Slot::pendingMount` and activated automatically in `read_sector()` and `write_sector()` if needed.

### YAML Format

```yaml
mounts:
  - slot: 1
    uri: "sd:/disks/boot.atr"
    mode: "auto"
    enabled: true
  - slot: 2
    uri: "tnfs://192.168.1.100:16384/atari/games.atr"
    mode: "ro"
    enabled: true
```

### URI Resolution and Authority Preservation

`StorageManager::resolveUri()` now preserves authority (host:port) for schemes like TNFS and HTTP:

- Input: `tnfs://192.168.1.100:16384/atari/disk.atr`
- Output: filesystem=`tnfs`, path=`tnfs://192.168.1.100:16384/atari/disk.atr`

This ensures the TNFS filesystem can connect to the correct host and port.

### Applying Config Mounts

The `apply_config_mounts()` function in `fujinet/fs/mount_applier.h` iterates through config mounts and mounts each to the corresponding disk slot:

1. Skips disabled mounts (`enabled: false`)
2. Skips empty URIs
3. Uses `effective_slot()` to resolve slot index
4. Resolves URI via `StorageManager::resolveUri()`
5. Calls `DiskService::mount()` with resolved filesystem and path

This runs during app startup after DiskDevice registration in both POSIX and ESP32 entry points.

### Disk Image Types and Lazy Loading

The lazy loading mechanism is **orthogonal to file type** - it works for any resource, not just disk images. Here's how different file types integrate:

**Architecture layers:**
```
DiskService (lazy mount handling)
    │
    ├─ read_sector() / write_sector()
    │   │
    │   ├─ Check pending mount → resolve URI → StorageManager
    │   │                                      │
    │   │                                      ▼
    │   │                               IFileSystem (TNFS/SD/HTTP/...)
    │   │                                      │
    │   │                                      ▼
    │   │                               IFile (opened file)
    │   │
    │   └─ If no image: ImageRegistry.create() → IDiskImage (ATR, SSD, XEX, ...)
              │
              ▼
         DiskService::mount() called internally
```

**How it works:**
1. **Lazy activation is transparent to file type** - The `StorageManager::resolveUri()` opens the file regardless of what it is
2. **`ImageRegistry` handles format detection** - Given a file path + optional type hint, it creates the appropriate `IDiskImage`
3. **`IDiskImage` abstracts the format** - Whether it's ATR, SSD, DSD, or XEX:
   - `mount()`: Open file, parse header, set geometry
   - `read_sector()`: Return sector data
   - `write_sector()`: Save changes back

**Future: XEX Boot Images (Atari)**

To support Atari XEX boot images (like the legacy `MediaTypeXex` in FujiNet firmware):

1. Create `XexDiskImage` implementing `IDiskImage`:
   - `mount()`: Load picoboot.bin (small boot loader), wrap XEX content in fake disk geometry
   - `read_sector(lba)`: Return picoboot for sectors 0-1, delegate XEX read for others
   - `write_sector()`: Not supported (XEX typically read-only)

2. Register in `ImageRegistry`:
   ```cpp
   registry.register(ImageType::Xex, make_xex_disk_image);
   ```

3. **Lazy loading just works** - TNFS opens the file on first access, ImageRegistry creates the appropriate image type

**Key insight:** The lazy loading mechanism doesn't care what the file contains - it ensures the file is opened when first accessed, then delegates format handling to the appropriate `IDiskImage` implementation.

---

## 13. Session Handover Checklist

Use this quick list at the start/end of a filesystem-related session:

1. Refresh generated source lists:
   - `./scripts/update_cmake_sources.py`
2. Validate POSIX build + unit tests:
   - `./build.sh -cp fujibus-pty-debug`
3. Validate ESP32 build:
   - `./build.sh -b`
4. If touching path parsing, run/extend:
   - `tests/test_path_resolver.cpp`
5. If touching TNFS path behavior, verify both:
   - `integration-tests/steps/35_tnfs.yaml` (UDP)
   - `integration-tests/steps/36_tnfs_tcp.yaml` (TCP)
6. If adding/modifying config mount behavior, verify:
   - `tests/test_fuji_config_yaml.cpp`
   - `tests/test_storage_manager.cpp`

When adding a new URI family (for example SMB), prefer:

- new handler class under `include/fujinet/fs/path_resolvers/`
- matching implementation under `src/lib/path_resolvers/`
- resolver registration in `PathResolver`
- table-style tests in `tests/test_path_resolver.cpp`

---
