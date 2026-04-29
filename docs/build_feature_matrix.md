# Build Feature Matrix

This document captures optional build-time backend combinations that should continue to compile cleanly, especially on POSIX where `libcurl` and OpenSSL can be enabled independently.

It is intended as a concise engineering reference and CI/local verification checklist, not as general project bootstrap documentation.

## POSIX Feature Flags

- `FN_WITH_CURL`
  - Enables the POSIX HTTP/HTTPS backend (`HttpNetworkProtocolCurl`)
- `FN_WITH_OPENSSL`
  - Enables the POSIX TLS stream backend (`TlsNetworkProtocolPosix`)
  - Also enables POSIX additive FujiNet test-CA injection for the curl HTTPS backend

## FujiNet Test CA Trust

HTTPS/TLS trust for the FujiNet test CA is now built into the supported runtime paths:

- POSIX: additive trust, alongside normal/public trust roots
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
   - FujiNet test-CA additive injection for curl HTTPS is unavailable in this build

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
