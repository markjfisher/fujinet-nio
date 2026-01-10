# fujinet-nio bootstrap (new ChatGPT session)

This repo is **fujinet-nio**, a clean rewrite of FujiNet firmware. It targets multiple platforms (notably **POSIX** and **ESP32**) while keeping most logic **platform-agnostic**.

## What you should assume (core architecture rules)

- **Platform-agnostic first**: device logic lives under `include/fujinet/` + `src/lib/` and should not contain platform `#ifdef`s.
- **Platform glue only**: platform differences live in `src/platform/<platform>/` and are expressed via factories/registries, not preprocessor conditionals.
- **Registration over branching**: new device/image/protocol types are registered via **init/registry** APIs (e.g. `register_*_device`, `make_default_*_registry`).
- **Binary protocols**: host↔device commands are little-endian binary payloads over the IO bus, exposed as `VirtualDevice` implementations.
- **Tests**: keep unit tests fast and deterministic (doctest). Integration tests (Python) verify end-to-end protocol behavior.

## Start here (high-signal docs)

- **Architecture overview**: `docs/architecture.md`
- **Protocol references**:
  - `docs/protocol_reference.md`
  - `docs/network_device_protocol.md` (good exemplar for a v1 binary protocol doc)
- **Diagnostics framework**: `docs/diagnostics.md`

## Key concepts and where they live

- **Virtual devices (host protocol endpoints)**:
  - `include/fujinet/io/devices/virtual_device.h`
  - `include/fujinet/io/core/io_message.h` (`IORequest`, `IOResponse`, `StatusCode`)
- **Wire device IDs**:
  - `include/fujinet/io/protocol/wire_device_ids.h`
- **Core registration entrypoint(s)**:
  - `include/fujinet/core/device_init.h` (`register_*_device(...)`)
- **Storage/filesystems**:
  - `include/fujinet/fs/storage_manager.h`
  - `include/fujinet/fs/filesystem.h`
- **Diagnostics**:
  - `include/fujinet/diag/diagnostic_provider.h`
  - `include/fujinet/diag/diagnostic_registry.h`

## Exemplars to copy (existing devices)

- **NetworkDevice** (binary protocol + backend split):
  - headers: `include/fujinet/io/devices/network_device.h`, `include/fujinet/io/devices/network_commands.h`
  - impl: `src/lib/network_device.cpp`, `src/lib/network_device_init.cpp`
  - platform registries: `src/platform/posix/network_registry.cpp`, `src/platform/esp32/network_registry.cpp`
  - doc: `docs/network_device_protocol.md`

- **FileDevice** (filesystem operations over protocol):
  - header: `include/fujinet/io/devices/file_device.h`
  - impl: `src/lib/file_device.cpp`, `src/lib/file_device_init.cpp`

- **DiskDevice** (service + format registry + protocol):
  - protocol/device: `include/fujinet/io/devices/disk_device.h`, `src/lib/disk_device.cpp`, `src/lib/disk_device_init.cpp`
  - disk subsystem: `include/fujinet/disk/`, `src/lib/disk/`
  - platform registry: `src/platform/posix/disk_registry.cpp`, `src/platform/esp32/disk_registry.cpp`
  - doc: `docs/disk_device_protocol.md`

## How to add a new device (minimal checklist)

- **Define a wire ID** in `include/fujinet/io/protocol/wire_device_ids.h`.
- **Define commands/codecs** in `include/fujinet/io/devices/<device>_commands.h` (+ optional `<device>_codec.h`).
- **Implement the `VirtualDevice`** in:
  - header: `include/fujinet/io/devices/<device>.h`
  - source: `src/lib/<device>.cpp`
- **Add a registration function** (no platform `#ifdef`):
  - header: `include/fujinet/core/device_init.h` (declare `register_<device>_device(...)`)
  - source: `src/lib/<device>_init.cpp` (construct device + any platform-provided registries)
- **Provide platform registries/factories** (if needed):
  - `include/fujinet/platform/<thing>_registry.h` (platform-agnostic interface)
  - `src/platform/posix/<thing>_registry.cpp`
  - `src/platform/esp32/<thing>_registry.cpp`
- **Hook into the app/core init** by calling your `register_<device>_device(...)` from the platform main (e.g. `src/app/main_posix.cpp`, `src/app/main_esp32.cpp`) in the same style as other devices.
- **Document the protocol** in `docs/<device>_protocol.md` (copy the NetworkDevice doc style).
- **Add tests**:
  - unit/doctest: `tests/test_<device>*.cpp` (fast, deterministic)
  - integration (Python): `integration-tests/steps/*.yaml` + `py/fujinet_tools/*` protocol helpers if applicable

## What “no ifdefs” means in practice

- Device/business logic should compile for both targets without conditional compilation.
- Platform differences are expressed through:
  - injected interfaces (factories, registries, hooks)
  - platform-specific implementations in `src/platform/<platform>/`
  - selection performed at **build graph / registration time**, not with `#ifdef` in device code

## Prompt template for future sessions (copy/paste)

I’m working on `fujinet-nio` (multi-platform: POSIX + ESP32). Please follow these rules:
- Keep new device logic platform-agnostic in `include/` + `src/lib/`. No `#ifdef` in device/service code.
- Put platform-specific glue in `src/platform/<platform>/` via registries/factories.
- Mirror existing device patterns (`NetworkDevice`, `FileDevice`, `DiskDevice`).
- Use `VirtualDevice` + binary little-endian payloads; map failures to `StatusCode`.
- Add doctest unit tests + (when applicable) Python integration tests.
- Update docs and link from `docs/architecture.md` when adding a new subsystem.
