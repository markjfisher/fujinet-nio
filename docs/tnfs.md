
# TNFS Implementation

## Overview

This document describes the TNFS (Tiny Network File System) implementation for fujinet-nio. TNFS is a lightweight network file system protocol designed for 8-bit computers and other resource-constrained devices.

## Implementation Architecture

The TNFS implementation consists of several key components:

### 1. ITnfsClient Interface

The `tnfs::ITnfsClient` interface (defined in `include/fujinet/tnfs/tnfs_protocol.h`) provides the core API for interacting with a TNFS server. This interface abstracts the underlying communication protocol (UDP), allowing for potential future implementations using other transport mechanisms.

### 2. UdpTnfsClient

The `UdpTnfsClient` class (defined in `src/lib/tnfs/tnfs_udp_client.cpp`) implements the `ITnfsClient` interface using UDP as the transport. It provides methods for:
- Mounting and unmounting filesystems
- Directory operations (list, create, remove)
- File operations (open, close, read, write, seek, tell)
- Stat operations (check if path exists, is directory, get file info)

### 3. TnfsFileSystem

The `TnfsFileSystem` class (defined in `src/lib/fs/tnfs_filesystem.cpp`) adapts the `ITnfsClient` interface to the `IFileSystem` interface used by the fujinet-nio core. This allows TNFS servers to be treated as standard filesystems within the fujinet-nio ecosystem.

### 4. TnfsFile

The `TnfsFile` class (defined in `src/lib/fs/tnfs_filesystem.cpp`) implements the `IFile` interface for TNFS files. It provides streaming read and write operations, seek and tell functionality, and flush support.

## UDP Channel

The UDP channel implementation (defined in `src/platform/posix/udp_channel.cpp`) provides a platform-specific implementation of the `Channel` interface for UDP communication. It supports:
- Non-blocking socket operations
- Available check for incoming data
- Reading from and writing to the socket

The UDP channel is used by the `UdpTnfsClient` to communicate with the TNFS server.

## Usage

To use the TNFS filesystem, you first need to create an instance of a TNFS client. For UDP communication, you would use:

```cpp
auto udpChannel = fujinet::platform::create_udp_channel("example.com", 16384);
auto tnfsClient = fujinet::tnfs::make_udp_tnfs_client(std::move(udpChannel));

if (tnfsClient->mount("/")) {
    auto filesystem = fujinet::fs::make_tnfs_filesystem(std::move(tnfsClient));
    // Use the filesystem for file operations
}
```

Alternatively, you can use the convenience function:

```cpp
auto filesystem = fujinet::fs::make_tnfs_filesystem("example.com", 16384, "/", "user", "password");
```

## Platform Support

Currently, the TNFS implementation supports:
- POSIX platforms (Linux, macOS, etc.) with UDP communication
- The implementation is designed to be platform-agnostic, with platform-specific code isolated in the `platform` directory

## Testing

The TNFS implementation is currently in a preliminary state and has not been tested with a real TNFS server. The implementation needs more work before it can be properly tested.

## Future Enhancements

Potential future enhancements include:
- Support for other transport mechanisms (e.g., TCP)
- Caching for improved performance
- Error recovery and retrying
- Additional directory and file operations
