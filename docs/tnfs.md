
# TNFS Implementation

## Overview

This document describes the TNFS (Tiny Network File System) implementation in `fujinet-nio`.
TNFS is integrated as a dynamic filesystem provider, so endpoints are resolved from URI/path at file-operation time rather than being hardcoded at startup.

Key outcomes from recent work:

- no startup hardcoding of TNFS host/port in app entrypoints
- dynamic endpoint/session resolution from requested path/URI
- UDP and TCP transports supported on both POSIX and ESP32
- console path handling for TNFS URI forms delegated to path resolvers

## Implementation Architecture

The TNFS implementation consists of these key components:

### 1. `ITnfsClient` Interface

The `tnfs::ITnfsClient` interface (in `include/fujinet/tnfs/tnfs_protocol.h`) provides the core TNFS API used by the filesystem adapter.

### 2. Common TNFS Client Logic (`CommonTnfsClient`)

`src/lib/tnfs/tnfs_client_common.h` implements shared protocol behavior used by both UDP and TCP TNFS clients:
- mount/unmount
- stat/exists/isDirectory
- directory operations (opendir/readdir/closedir)
- file operations (open/close/read/write/lseek/tell)

`src/lib/tnfs/tnfs_udp_client.cpp` and `src/lib/tnfs/tnfs_tcp_client.cpp` are thin wrappers that provide transport-specific channels and reuse the common logic.

### 3. `TnfsFileSystem`

The `TnfsFileSystem` class (defined in `src/lib/fs/tnfs_filesystem.cpp`) adapts the `ITnfsClient` interface to the `IFileSystem` interface used by the fujinet-nio core. This allows TNFS servers to be treated as standard filesystems within the fujinet-nio ecosystem.

Important behavior:
- Endpoints are parsed from the request path/URI (`host`, `port`, `mountPath`, optional credentials).
- TNFS sessions are cached and reused per endpoint+transport.
- The filesystem mounts on demand when first used, not at app startup.
- session keys include transport (`useTcp`) so UDP/TCP endpoints are isolated correctly.

### 4. `TnfsFile`

The `TnfsFile` class (defined in `src/lib/fs/tnfs_filesystem.cpp`) implements the `IFile` interface for TNFS files. It provides streaming read and write operations, seek and tell functionality, and flush support.

## URI And Path Format

When using FileDevice commands, pass the filesystem name as `tnfs`, and pass endpoint/path in URI-style path form:

- UDP (default):
  - `list tnfs //localhost:16384/`
  - `stat tnfs //localhost:16384/test.txt`
- Also accepted UDP URI:
  - `tnfs://localhost:16384/test.txt`
- TCP URI forms:
  - `tnfs+tcp://localhost:16384/test.txt`
  - `tnfstcp://localhost:16384/test.txt`
  - `tnfs-tcp://localhost:16384/test.txt`

Console path resolver behavior:

- all TNFS URI schemes above are routed to filesystem `tnfs`
- `tnfs://...` selects UDP by default
- `tnfs+tcp://...`/aliases select TCP
- relative path traversal while in a TNFS endpoint preserves authority (`host[:port]`)

Using `tnfs` as both fs name and URI scheme can produce display output like `tnfs:tnfs://...` in CLI output. This is only formatting from the helper script, not a protocol issue.

## Transport Selection Details

Transport is selected from URI scheme at endpoint parse time:

- UDP: `tnfs://...` or endpoint-only `//host:port/...`
- TCP: `tnfs+tcp://...`, `tnfstcp://...`, `tnfs-tcp://...`

Platform factories (`platform/posix/fs_factory.cpp`, `platform/esp32/fs_factory.cpp`) create the appropriate TNFS client/channel from resolved endpoint data.

## Platform Support

- POSIX uses dynamic TNFS filesystem factory and supports UDP (default) and TCP TNFS clients.
- ESP32 uses the same dynamic TNFS filesystem model and supports UDP and TCP TNFS clients.
- Core TNFS protocol behavior (including STAT decoding and tell/lseek logic) is shared via `CommonTnfsClient`, so protocol fixes apply to both platforms.

## Testing

Current integration tests cover:
- list root directory
- stat file metadata
- read file content
- read from subdirectory
- write file and read it back

See:
- `integration-tests/steps/35_tnfs.yaml` (UDP)
- `integration-tests/steps/36_tnfs_tcp.yaml` (TCP)

Path resolver coverage:

- `tests/test_path_resolver.cpp` validates URI routing and TNFS relative path behavior.

For local test services, ensure TNFS is published on both UDP and TCP port `16384` when TCP tests are enabled.

For ESP32 integration runs, use a reachable service IP (not localhost), e.g.:

`./integration-tests/run_integration.py --port /dev/ttyUSB0 --esp32 --ip 192.168.1.101 --fs sd0`

## Notes

- UDP endpoint resolution prefers IPv4 addresses when available (important for localhost TNFS daemons bound only to IPv4).
- TNFS STAT parsing follows wire layout:
  - `status, mode, uid, gid, size, atime, mtime, ctime`
  - directory detection is derived from `mode` type bits.
- default TNFS file creation permissions are set to `0644` to avoid unreadable `000` files in test workflows.
- test service startup runs TNFS container with host UID/GID to avoid root-owned artifacts in shared host test dirs.
- **Config-driven TNFS mounts**: TNFS URIs in YAML config mounts (e.g., `tnfs://192.168.1.100:16384/atari/disk.atr`) are automatically applied to disk slots at startup. The `StorageManager::resolveUri()` preserves the full URI including authority (host:port), allowing TNFS to connect to the correct endpoint.

## Session Handover Checklist

Use this sequence before handing off or starting a clean TNFS session:

1. Regenerate generated CMake source lists:
   - `./scripts/update_cmake_sources.py`
2. Build and run unit tests:
   - `./build.sh -cp fujibus-pty-debug`
3. Build ESP32 target:
   - `./build.sh -b`
4. Run TNFS integration groups (UDP + TCP):
   - `integration-tests/run_integration.py --port <PORT> --steps-dir integration-tests/steps --only-group TNFS`
5. For ESP32 integration runs, always pass reachable TNFS host IP:
   - `integration-tests/run_integration.py --port /dev/ttyUSB0 --esp32 --ip <LAN_IP> --fs sd0`
6. If write/read tests fail unexpectedly, verify:
   - TNFS service is exposing both UDP/TCP `16384`
   - shared test directory ownership/perms (UID/GID mapping and non-`000` files)
