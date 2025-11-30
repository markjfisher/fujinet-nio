# fujinet-nio

**fujinet-nio** is a modern, clean re-implementation of FujiNet I/O services in C++.

This project is a fresh start, intentionally designed to:
- avoid legacy architectural constraints,
- use modern C++ with strong memory-safety guarantees,
- support multiple deployment targets from a single codebase, and
- be testable, extensible, and maintainable long-term.

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

