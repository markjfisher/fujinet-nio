# fujinet-nio

## About

**fujinet-nio** is a modern, clean re-implementation of FujiNet I/O services in C++.

This project is a fresh start, intentionally designed to:
- avoid legacy architectural constraints,
- use modern C++ with strong memory-safety guarantees,
- support multiple deployment targets from a single codebase, and
- be testable, extensible, and maintainable long-term.

## Building

Read the [developer onboarding docs](docs/developer_onboarding.md)

## Building TL;DR:

### prerequisites

The following are required:

- Python (with `virtualenv` & `uv`)
- [PlatformIO](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide) (abbreviated as "PIO" - VSCode extension recommended)
- [ESP32-S3 toolchain](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html) (auto-installed by PIO)

Install prerquisites from packages:

>**_NOTE:_** PlatformIO Core can be installed from packages but they may be old versions.  Using the VSCode extension will install the current version instead.
- Ubuntu (optionally add `platformio` to install PlatformIO Core)

  ```sh
  apt install --no-install-recommends python3 python3-venv python3-uv cmake build-essential # platformio
  ```

- Arch (optionally add `platformio-core` to install PlatformIO Core)

  ```sh
  pacman -S python python-uv cmake base-devel # platformio-core
  ```

### esp32 initial setup

You need an appropriate `sdkconfig.defaults` and `platformio.ini` file for your board type.
This is done by the `./build.sh` script, and should not be done manually as it will be overwritten on each build.

The build script will pull together relevant sections for both files from templates, you only need to provide local
overrides (see below).

```bash
# list the board types that can be built
$ ./build.sh -S
fujibus-usbcdc-consolecdc-s3-wroom-1-n16r8
...

# setup the build environment for the board type
# WARNING: this will overwrite the sdkconfig.local.defaults file, and platformio.local.ini files
# and should only be done once unless you want to reset the build environment
$ ./build.sh -s fujibus-usbcdc-consolecdc-s3-wroom-1-n16r8
```

### local configuration files

The files `sdkconfig.defaults` and `platformio.ini` are generated from the board type and should not be edited directly as they are overwritten on each build.

There are 2 editable config files that you can use to affect the build.

- `sdkconfig.local.defaults` - this is appended to the `sdkconfig.defaults` generated file when you run a build.
- `platformio.local.ini` - values in here are merged into the `platformio.ini` file when you run a build.

### build the firmware

```bash
# run a clean/build/upload/monitor for pio target
$ ./build.sh -cbum
```

### posix build

The posix build is done with cmake, and presets.

You can view the posix presets with

```bash
$ ./scripts/build -pS
Available profiles:
fujibus-pty-debug   - FujiBus over PTY (Debug)
fujibus-pty-release - FujiBus over PTY (Release)
...
```

#### build the posix target

```bash
# clean and build the posix target. Omit the -c to skip the clean step
$ ./build.sh -cp fujibus-pty-debug
```

### build locations

For the platformio builds, the build files are located in the `.pio` directory at the root of the project.

For the posix builds, the build files are located in the `build` directory at the root of the project, and under the subfolder for the target name, e.g. `build/fujibus-pty-debug`.

---

## Goals

- ✅ **Modern C++ (C++20+)**
  - RAII, smart pointers, value semantics, minimal globals
  - Clear ownership and lifetime boundaries

- ✅ **Clean I/O Architecture**
  - Transport-agnostic core
  - Virtual devices decoupled from buses and protocols
  - Explicit routing instead of implicit global state

- ✅ **Multi-target Support**
  - ESP32 (PlatformIO)
  - POSIX applications (Linux / macOS / Windows)
  - Static/dynamic library for embedding in emulators or other software
  - WebAssembly (future goal, for web-based UI & testing)

- ✅ **Test-first Development**
  - Unit tests from day one
  - No “untestable singleton” designs
  - Deterministic, platform-independent core logic

- ✅ **Simple, Type-Safe Configuration**
  - Strongly typed config data
  - Serialization/deserialization without custom INI glue
  - Easy to add new configuration fields without boilerplate

---

## What This Is (and Isn’t)

- This **is not** a drop-in replacement for existing FujiNet firmware (yet!)
- This **is** a new foundation that can:
  - reuse ideas from existing projects,
  - host compatible protocols and virtual devices,
  - and eventually power multiple front ends and platforms.

---

## License

See [LICENSE](LICENSE)
