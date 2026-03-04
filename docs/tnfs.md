
# TNFS Implementation

## Overview

This document describes the TNFS (Tiny Network File System) implementation in `fujinet-nio`.
TNFS is integrated as a dynamic filesystem provider, so endpoints are resolved from URI/path at file-operation time rather than being hardcoded at startup.

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
- TNFS sessions are cached and reused per endpoint.
- The filesystem mounts on demand when first used, not at app startup.

### 4. `TnfsFile`

The `TnfsFile` class (defined in `src/lib/fs/tnfs_filesystem.cpp`) implements the `IFile` interface for TNFS files. It provides streaming read and write operations, seek and tell functionality, and flush support.

## URI And Path Format

When using FileDevice commands, pass the filesystem name as `tnfs`, and pass endpoint/path in URI-style path form:

- Preferred:
  - `list tnfs //localhost:16384/`
  - `stat tnfs //localhost:16384/test.txt`
- Also accepted:
  - `tnfs://localhost:16384/test.txt`

Using `tnfs` as both fs name and URI scheme can produce display output like `tnfs:tnfs://...` in CLI output. This is only formatting from the helper script, not a protocol issue.

## Platform Support

- POSIX uses dynamic TNFS filesystem factory and supports UDP (default) and TCP TNFS clients.
- ESP32 uses the same dynamic TNFS filesystem model and currently uses UDP TNFS client creation in the platform factory.
- Core TNFS protocol behavior (including STAT decoding and tell/lseek logic) is shared via `CommonTnfsClient`, so protocol fixes apply to both platforms.

## Testing

Current integration tests cover:
- list root directory
- stat file metadata
- read file content
- read from subdirectory
- write file and read it back

See `integration-tests/steps/35_tnfs.yaml`.

## Notes

- UDP endpoint resolution prefers IPv4 addresses when available (important for localhost TNFS daemons bound only to IPv4).
- TNFS STAT parsing follows wire layout:
  - `status, mode, uid, gid, size, atime, mtime, ctime`
  - directory detection is derived from `mode` type bits.
