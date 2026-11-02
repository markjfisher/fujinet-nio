# Build Feature Matrix

This document captures optional build-time backend combinations that should continue to compile cleanly, especially on POSIX where `libcurl` and OpenSSL can be enabled independently.

It is intended as a concise engineering reference and CI/local verification checklist, not as general project bootstrap documentation.

## POSIX Feature Flags

- `FN_WITH_CURL`
  - Enables the POSIX HTTP/HTTPS backend (`HttpNetworkProtocolCurl`)
- `FN_WITH_OPENSSL`
  - Enables the POSIX TLS stream backend (`TlsNetworkProtocolPosix`)
  - Lets the curl HTTPS backend add the FujiNet test CA to curl's own `SSL_CTX`, when curl uses this same OpenSSL
- `FN_WITH_HOTSYNC`
  - Builds Palm HotSync ([`hotsync.md`](hotsync.md)): `src/lib/hotsync/`, its diagnostics and `tests/test_hotsync.cpp`
  - A CMake variable, not a compiler flag, and the build selects sources by it: HotSync's, or only `src/lib/hotsync_init_off.cpp`, a `start_hotsync()` that does nothing. No source carries a feature guard. `scripts/update_cmake_sources.py` generates both lists, under `if(FN_WITH_HOTSYNC)`, in `CMakeLists_posix.cmake` and `src/CMakeLists.txt`
  - POSIX: defaults on for the `fujibus-pty-*` and `fujibus-rs232-*` presets and off for the rest
  - ESP32: a board turns it on with `-D FN_WITH_HOTSYNC=ON` in the `board_build.cmake_extra_args` of its `pio-build/ini/platforms/*.ini` (the FN-RS232 boards do), so the choice is made before `idf_component_register`. Other boards compile no HotSync source, which matters on ESP32: linker garbage collection alone would leave every object's `.eh_frame`, about 14 KB for HotSync

## FujiNet Test CA Trust

HTTPS/TLS trust for the FujiNet test CA is now built into the supported runtime paths:

- POSIX: additive trust, alongside normal/public trust roots
  - The curl HTTPS backend adds the test CA to curl's `SSL_CTX` only when curl's TLS library is the OpenSSL NIO links (`curl_version_info()->ssl_version` names the same release). Otherwise it hands curl an in-memory CA bundle (`CURLOPT_CAINFO_BLOB`, curl 7.77+): the test CA alone in test-CA-only mode, or, to keep the platform roots, curl's default CA file (`CURLINFO_CAINFO`, curl 7.84+) followed by the test CA. macOS's system curl is the common case: it runs on LibreSSL, so its `SSL_CTX` cannot go to Homebrew OpenSSL.
  - If curl has no default CA file (it uses the system trust store) or is too old, the test CA cannot be added in the platform-plus-test-CA mode: a warning is logged and only the platform roots are trusted.
- ESP32: built-in test-CA trust path for local FujiNet test services
- URL query hacks such as `?testca=1` are intentionally not part of the supported workflow

## POSIX Compile Matrix

Useful compile combinations to keep healthy:

1. `FN_WITH_CURL=1`, `FN_WITH_OPENSSL=1`
   - Full POSIX HTTP/HTTPS + TLS support
   - Preferred developer configuration

2. `FN_WITH_CURL=1`, `FN_WITH_OPENSSL=0`
   - HTTP/HTTPS backend enabled
   - TLS stream backend disabled
   - curl HTTPS gets the FujiNet test CA as an in-memory CA bundle (`CURLOPT_CAINFO_BLOB`: curl 7.77+, or 7.84+ to keep the platform roots)
   - CI runs this combination (`.github/workflows/tests.yml`)

3. `FN_WITH_CURL=0`, `FN_WITH_OPENSSL=1`
   - No POSIX HTTP/HTTPS backend
   - TLS stream backend enabled

4. `FN_WITH_CURL=0`, `FN_WITH_OPENSSL=0`
   - Minimal fallback build with neither optional POSIX crypto/network backend enabled

## Recommended Compile Checks

The following commands are useful local compile-only checks.

### Full POSIX Feature Set

```bash
cmake -S . -B build/check-full -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFN_BUILD_FUJIBUS_PTY=ON \
  -DFN_BUILD_POSIX_APP=ON \
  -DFN_BUILD_TESTS=ON \
  -DFN_WITH_CURL=1 \
  -DFN_WITH_OPENSSL=1
cmake --build build/check-full
```

### Curl Without OpenSSL

```bash
cmake -S . -B build/check-curl-noopenssl -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFN_BUILD_FUJIBUS_PTY=ON \
  -DFN_BUILD_POSIX_APP=ON \
  -DFN_BUILD_TESTS=ON \
  -DFN_WITH_CURL=1 \
  -DFN_WITH_OPENSSL=0
cmake --build build/check-curl-noopenssl
```

### OpenSSL Without Curl

```bash
cmake -S . -B build/check-nocurl-openssl -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFN_BUILD_FUJIBUS_PTY=ON \
  -DFN_BUILD_POSIX_APP=ON \
  -DFN_BUILD_TESTS=ON \
  -DFN_WITH_CURL=0 \
  -DFN_WITH_OPENSSL=1
cmake --build build/check-nocurl-openssl
```

### Neither Curl Nor OpenSSL

```bash
cmake -S . -B build/check-minimal -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFN_BUILD_FUJIBUS_PTY=ON \
  -DFN_BUILD_POSIX_APP=ON \
  -DFN_BUILD_TESTS=ON \
  -DFN_WITH_CURL=0 \
  -DFN_WITH_OPENSSL=0
cmake --build build/check-minimal
```

## What To Validate Functionally

For day-to-day development, the most important runtime validation remains:

1. POSIX with full features (`FN_WITH_CURL=1`, `FN_WITH_OPENSSL=1`)
2. ESP32 firmware build
3. Integration tests against both POSIX and ESP32 instances where semantics are expected to match

Not every compile-matrix variant needs full runtime integration coverage, but all of them should continue to compile.

## Related Docs

- `docs/context_bootstrap.md`
- `docs/http_filesystem.md`
