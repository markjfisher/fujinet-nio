# HTTP/HTTPS Filesystem

## Overview

`fujinet-nio` now exposes an HTTP-backed filesystem provider registered as `http`.
Like TNFS, it is a dynamic provider: the endpoint is resolved from the requested URL at file-operation time rather than being fixed at startup.

This allows disk images and other files to be fetched directly from ordinary web servers with URLs such as:

- `http://192.168.1.101/disks/game.atr`
- `https://example.com/atari/demo.xex`

The filesystem is intentionally read-only in this first version. It supports:

- `stat`
- `exists`
- `open` for read access

It does not support:

- directory listing
- directory creation/removal
- file deletion
- rename
- writing back to HTTP/HTTPS resources

## Architecture

The HTTP filesystem is implemented in `src/lib/fs/http_filesystem.cpp` and adapts the existing platform HTTP network backends to the generic `IFileSystem` interface.

Key behavior:

- URLs are passed through unchanged as filesystem paths
- scheme selection is dynamic per request (`http` or `https`)
- POSIX uses the libcurl-backed network protocol
- ESP32 uses the ESP-IDF HTTP client backend
- the filesystem buffers the fetched response body in memory for read/seek operations

Platform-specific factories are provided in:

- `src/platform/posix/fs_factory.cpp`
- `src/platform/esp32/fs_factory.cpp`

Application bootstrap registers the provider in:

- `src/app/main_posix.cpp`
- `src/app/main_esp32.cpp`

## HTTPS Test CA

For local HTTPS testing with the FujiNet test certificate authority, configure trust in `fujinet.yaml` instead of adding URL query parameters:

```yaml
tls:
  trust_test_ca: true
```

This applies to HTTPS validation during both `FHOST` probing and later file access. URLs remain normal resource URLs such as:

- `https://192.168.1.101:18443/bbc/impetus_mode7.ssd`

Using URL query parameters for trust policy is intentionally avoided because they become part of the stored path and break relative URL composition.

## URI And Path Format

When using FileDevice commands, pass full HTTP or HTTPS URLs:

- `stat http http://example.com/disks/game.atr`
- `read http https://example.com/demo.xex`

StorageManager resolution preserves the full URL, including authority and scheme. The provider is always registered under filesystem name `http`, and handles both:

- `http://...`
- `https://...`

Path resolver behavior:

- `http://...` and `https://...` route to filesystem `http`
- relative traversal from an HTTP current working directory preserves scheme and authority
- absolute paths replace only the URL path component and preserve scheme/authority

## Status And Open Semantics

`stat()` prefers an HTTP `HEAD` request and falls back to `GET` when needed. If the server does not provide `Content-Length` on `HEAD`, the fallback `GET` determines the size from the response body.

`open(..., "rb")` performs an HTTP `GET`, buffers the response body, and exposes it through the normal `IFile` interface.

Only HTTP 2xx responses are treated as successful filesystem access.

## Platform Support

- POSIX: supported via `HttpNetworkProtocolCurl`
- ESP32: supported via `HttpNetworkProtocolEspIdf`

Because the filesystem reuses the same network protocol registry used by `NetworkDevice`, HTTP/HTTPS behavior remains aligned across both the filesystem and network-service paths.

## Testing

Unit coverage includes:

- HTTP filesystem stat/open/read behavior
- HTTPS URL acceptance
- read-only enforcement
- StorageManager HTTPS URI preservation
- PathResolver handling for HTTP/HTTPS URIs and relative traversal

Relevant tests:

- `tests/test_http_filesystem.cpp`
- `tests/test_http_path_resolver.cpp`
- `tests/test_storage_manager.cpp`
- `tests/test_path_resolver.cpp`

## Session Handover Checklist

1. Regenerate generated CMake source lists:
   - `./scripts/update_cmake_sources.py`
2. Build and run unit tests:
   - `./build.sh -cp fujibus-pty-debug`
3. Build ESP32 target:
   - `./build.sh -b`

For optional POSIX feature-toggle build combinations (`FN_WITH_CURL`, `FN_WITH_OPENSSL`), see `docs/build_feature_matrix.md`.
