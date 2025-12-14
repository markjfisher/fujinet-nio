# FujiNet-NIO: Network OS (NOS) Alignment Notes

*Working document — intended as a “compass” for future implementation decisions.*

This document captures how **fujinet-nio** should evolve to support FujiNet’s **NOS-style “network-only DOS”** workflows, while remaining faithful to the current NIO architecture (Channel → Transport → Core → VirtualDevices) and the filesystem abstractions already in place.

It is not a commitment to a specific implementation timeline. It is a reference design to keep future work coherent.

---

## 1. Background and Goal

FujiNet NOS treats “network” as a first-class storage environment: users interact with **network locations** as if they were disks/directories, and typically expect multiple **independent N slots** (e.g. N1..N8) each holding its own working directory and connection context.

For fujinet-nio, the long-term goal is:

- Support **multiple URL schemes** (http/https/tcp/udp/ftp/tnfs/smb/…)
- Provide **filesystem-like semantics** for schemes that can support them
- Preserve transport-agnostic device protocols
- Keep platform details in platform layers (POSIX / ESP32)

---

## 2. Key Architectural Principle

### “Network” is two things, and we should not conflate them:

#### A) Transaction-style networking (request/response, streams)
Examples:
- HTTP GET/HEAD returning status/headers/body
- POST/PUT upload streaming
- TCP stream I/O

This maps naturally to the **NetworkDevice** protocol:
- `Open/Info/Read/Write/Close`
- one handle ≈ one request/stream session

#### B) Filesystem-style networking (paths, directories, file ops)
Examples:
- current working directory
- directory listing
- open/read/write file by path
- rename/delete/mkdir/rmdir

This maps naturally to the **filesystem abstraction** (`IFileSystem/IFile`) and **FileDevice** protocol:
- `Stat/ListDirectory/ReadFile/WriteFile`
- stable “filesystem name” selection in `StorageManager`

**Compass statement:**
> Keep NetworkDevice as the “network transaction” API, and implement NOS-style behavior via filesystem semantics (FileDevice + network-backed filesystems), not by overloading NetworkDevice.

---

## 3. Target End-State Overview

### 3.1 One NetworkDevice, multiple sessions
- We keep one `NetworkDevice` instance.
- It owns a session/handle table (already started).
- It selects protocol backends by URL scheme via a registry (already implemented).

This satisfies:
- multiple concurrent network operations
- a single wire device ID for “network services”
- a clean platform injection point for backend implementations

### 3.2 Network-backed filesystems registered in StorageManager
To support NOS-style behavior, we introduce network-backed filesystem instances that implement `IFileSystem`:

- `TnfsFileSystem`
- `SmbFileSystem`
- `FtpFileSystem`
- (Optional) `HttpFileSystem` if you decide to represent web resources as files/directories in a limited way

These can be registered by platform bootstrap or by a “network mount manager” device.

---

## 4. The “N Slots” Concept (N1..N8)

### 4.1 What an N slot represents
Each N slot is an independent context with:
- a “current root” (often a URL + authority/credentials)
- a current working directory under that root
- scheme-specific state (connections, cookies, auth tokens, etc.)
- cached directory entries or file handles (optional)

### 4.2 How N slots map into NIO
We want N slots to be visible to the core as something addressable by name, without introducing platform special-cases.

**Recommended mapping:**
- Represent each N slot as an `IFileSystem` registered under a stable name:
  - `"n1"`, `"n2"`, … `"n8"`

Then:
- `FileDevice` can operate on `"n1"` exactly as it does `"sd0"` or `"flash"`.
- Host tooling can use the same FileDevice protocol against local and network-backed filesystems.

---

## 5. How to “Mount” or Configure an N Slot

We need a way to set `n1` to point at some network location (URL or server/share).

Two viable options:

### Option A: Dedicated device commands (preferred)
Add a small set of commands to a device (new or existing) that configure N slots:
- `SetSlotRoot(slot, url)`
- `GetSlotRoot(slot)`
- `SetSlotCwd(slot, path)`
- `GetSlotCwd(slot)`
- `SetSlotCredentials(slot, user, pass)` (or token)
- `ClearSlot(slot)`

This keeps configuration explicit and avoids overloading filesystem APIs.

### Option B: Virtual filesystem mount points
Treat `n1` as an initially-empty filesystem where writing a config file (e.g. `/MOUNT.URL`) triggers mount configuration.
This is “cute” but can be brittle and harder to make robust.

**Compass statement:**
> Prefer explicit device commands for slot configuration, because it’s easier to make correct on constrained devices and simple for 8-bit clients.

---

## 6. Capability Model: Not all schemes are equal

Different schemes support different filesystem operations:

- SMB/TNFS/FTP: generally support directory listing + full file CRUD
- HTTP: naturally supports read (GET) and metadata (HEAD), but directory listing is non-standard
- TCP/UDP: not filesystem-like at all

We should model this explicitly, not implicitly.

**Proposal:**
- Each network-backed filesystem reports capabilities:
  - canListDirectories
  - canReadFiles
  - canWriteFiles
  - canRename
  - canDelete
  - canMakeDirectories
  - etc.

Then:
- unsupported operations return a consistent status (e.g. `Unsupported`) rather than ad-hoc errors.

---

## 7. Relationship Between NetworkDevice and Network Filesystems

These can coexist cleanly:

- NetworkDevice remains the “transaction engine” for:
  - HTTP fetches for applications
  - JSON-to-query transformation
  - raw streaming protocols

- Network-backed filesystems are “filesystem semantics” for:
  - NOS
  - file explorers
  - programmatic file access across protocols

They may share internal protocol backends or connection pools, but they do not have to share the same wire protocol.

**Compass statement:**
> Don’t force filesystem semantics into NetworkDevice just because both concepts are “network”.

---

## 8. Incremental Implementation Roadmap (non-binding)

This is a suggested progression that keeps architectural integrity.

### Stage 1 (now)
- Finish NetworkDevice multi-scheme backend plumbing
- Implement POSIX HTTP via libcurl
- Implement ESP32 HTTP via esp_http_client later

### Stage 2
- Implement a first network-backed filesystem where semantics are clear:
  - TNFS (strong fit to FujiNet heritage), or
  - SMB (common and useful)

Register it as `"tnfs0"` or `"smb0"` (static mounts) to validate the `IFileSystem` approach.

### Stage 3 (NOS alignment)
- Implement N slot concept as `"n1"`..`"n8"` `IFileSystem`s
- Add a device command set to mount/configure each slot
- Ensure FileDevice can fully operate on N slots

### Stage 4
- Add “quality of life” features:
  - caching / listing chunk stability
  - persistent slot config (config store integration)
  - credential storage policies

---

## 9. Non-Goals / Guardrails

- Do not introduce platform #ifdefs into core device protocols.
- Do not require legacy transports to achieve NOS alignment (legacy is valuable, but not required for the architecture direction).
- Do not make HTTP pretend to be a perfect filesystem if it isn’t (define a limited mapping if desired).

---

## 10. Summary: The Compass

- **NetworkDevice**: transaction/stream protocol (Open/Read/Write/Info/Close), multi-scheme via registry.
- **FileDevice + IFileSystem**: filesystem semantics, including network-backed filesystems.
- **N slots**: multiple independent filesystem contexts (`"n1"`..`"n8"`) configurable by explicit commands.
- **Capabilities**: explicit per-scheme support matrix, consistent error signaling.

This document should be updated whenever we introduce:
- N slot configuration mechanisms
- network-backed filesystem implementations
- new protocol capability requirements
- changes to FileDevice/NetworkDevice semantics that affect NOS alignment
