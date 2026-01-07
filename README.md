# fujinet-nio

## About

**fujinet-nio** is a modern, clean re-implementation of FujiNet I/O services in C++.

This project is a fresh start, intentionally designed to:
- avoid legacy architectural constraints,
- use modern C++ with strong memory-safety guarantees,
- support multiple deployment targets from a single codebase, and
- be testable, extensible, and maintainable long-term.

## Building TL;DR:

### esp32 initial setup

You need to create an appropriate `sdkconfig.defaults` and `platformio.ini` file for your board type.
This can be done with the `./build.sh` script, and should not be done manually as it will be overwritten on each build.

```bash
# list the board types that can be built
$ ./build.sh -S
fujibus-usbcdc-consolecdc-s3-wroom-1-n16r8
atari-sio-consolecdc-s3-wroom-1-n16r8

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
$ ./scripts/build_posix.sh -S
Available profiles:
fujibus-pty-debug   - FujiBus over PTY (Debug)
fujibus-pty-release - FujiBus over PTY (Release)
atari-release       - Atari SIO profile (Release)
lib-only            - library only (no app, tests on)
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

- This **is not** a drop-in replacement for existing FujiNet firmware.
- This **is** a new foundation that can:
  - reuse ideas from existing projects,
  - host compatible protocols and virtual devices,
  - and eventually power multiple front ends and platforms.

Compatibility is a *goal*, not a constraint.

---

## Repository Layout (Early Stage)

```text
fujinet-nio/
├── CMakeLists.txt          # Primary build system
├── platformio.ini          # ESP32 / embedded build support
├── include/                # Public headers (library API)
│   └── fujinet/
│       └── io/
│           └── core/
├── src/
│   ├── lib/                # Core library implementation
│   └── app/                # Application entry points (POSIX / ESP32)
├── tests/                  # Unit tests
└── README.md
```

This structure will grow as:

- transports (RS232, SIO, IEC, etc.) are added,
- virtual devices (disk, printer, clock, network, etc.) mature,
- and platform-specific bootstrap layers are introduced.

---

## Building (POSIX)

```
mkdir build
cd build
cmake ..
cmake --build .
./fujinet-nio
```

Tests:

```
ctest
```

## ESP32 / PlatformIO

PlatformIO support is scaffolded from the start:

```
pio run -e esp32dev
```

ESP32 entry points will be added under src/app/ as the core matures.

## Status

🚧 Early development
This repository currently provides:

- the project skeleton,
- build system setup,
- a minimal core I/O model,
- and smoke tests to ensure correctness from the beginning.

Expect rapid iteration.

---

## Contributing

Early contributions are discussion-driven. If you’re interested:

- architecture feedback is welcome,
- test coverage is encouraged,
- code should prioritize clarity over cleverness.

Documentation and comments matter as much as code.

## License

