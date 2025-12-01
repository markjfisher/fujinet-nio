# Build Profiles, Transports, and Avoiding `#ifdef` Hell

This document explains how **fujinet-nio** decides:

- which **machine profile** it is (Atari, generic, etc.),
- which **transport** to use (SIO, IDC, Serial, ...),
- and how we keep all of that from turning into `#ifdef` spaghetti.

The core idea is:

> All build flags are interpreted **once** in `build_profile.cpp`, and everywhere else we use **clean enums and data**, not preprocessor soup.

---

## Layers Overview

We split responsibilities into three layers:

1. **Core I/O (pure logic)**  
   - `IODeviceManager`, `RoutingManager`, `IOService`, `VirtualDevice`,
     `Channel`, `ITransport`, `Rs232Transport`, `SioTransport`, etc.  
   - Knows nothing about build flags, boards, or pinmaps.  
   - Works in both POSIX and ESP-IDF.

2. **Build Profile (machine + transport choice)**  
   - `build_profile.{h,cpp}`  
   - Turns **compile-time macros**
     (e.g. `FN_BUILD_ATARI`, `FN_BUILD_RS232`)
     into a **simple struct**:
     - which machine type
     - which primary transport
     - a human-friendly name

3. **Platform / Bootstrap (POSIX / ESP32 / board)**  
   - POSIX: `main_posix.cpp`, `platform/posix/*`  
   - ESP-IDF: `main_esp32.cpp`, `platform/esp32/*`  
   - They:
     - create a `FujinetCore`
     - create a platform-specific `Channel`
     - call a shared `setup_transports()`
     - register devices
     - drive `core.tick()` in a loop

Only layer 2 (build profile) and layer 3 (platform) are allowed to look at
build flags and board specifics.

---

## Build Profiles

### Public API

File: `include/fujinet/config/build_profile.h`

```cpp
#pragma once

#include <string_view>

namespace fujinet::config {

enum class Machine {
    Generic,
    Atari8Bit,
    C64,
    Apple2,
};

enum class TransportKind {
    SIO,
    IEC,
    SerialDebug,
};

struct BuildProfile {
    Machine           machine;
    TransportKind     primaryTransport;
    std::string_view  name;
};

BuildProfile current_build_profile();

} // namespace fujinet::config
```

### Mapping build flags → profile

File: `src/lib/build_profile.cpp`

```cpp
#include "fujinet/config/build_profile.h"

namespace fujinet::config {

BuildProfile current_build_profile()
{
#if defined(FN_BUILD_ATARI)
    return BuildProfile{
        .machine          = Machine::Atari8Bit,
        .primaryTransport = TransportKind::SIO,
        .name             = "Atari + SIO",
    };
#elif defined(FN_BUILD_RS232)
    return BuildProfile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::SerialDebug,
        .name             = "Generic + RS232",
    };
#else
    return BuildProfile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::PTY,
        .name             = "POSIX + PTY",
    };
#endif
}

} // namespace fujinet::config
```

This is the **only place** in the codebase where build macros are interpreted.

---

## Transport Setup (Bootstrap Layer)

### API

File: `include/fujinet/core/bootstrap.h`

```cpp
#pragma once

#include "fujinet/core/core.h"
#include "fujinet/config/build_profile.h"
#include "fujinet/io/core/channel.h"

namespace fujinet::core {

io::ITransport* setup_transports(
    FujinetCore& core,
    io::Channel& channel,
    const config::BuildProfile& profile
);

} // namespace fujinet::core
```

### Implementation

File: `src/lib/bootstrap.cpp`

```cpp
#include "fujinet/core/bootstrap.h"

#include "fujinet/io/transport/rs232_transport.h"

namespace fujinet::core {

io::ITransport* setup_transports(
    FujinetCore& core,
    io::Channel& channel,
    const config::BuildProfile& profile)
{
    using config::TransportKind;
    io::ITransport* primary = nullptr;

    switch (profile.primaryTransport) {
    case TransportKind::SerialDebug: {
        auto* t = new io::Rs232Transport(channel);
        core.addTransport(t);
        primary = t;
        break;
    }
    case TransportKind::SIO:
    case TransportKind::IEC:
        // Implement later
        break;
    case TransportKind::PTY: {
        auto* t = new io::Rs232Transport(channel);
        core.addTransport(t);
        primary = t;
        break;
    }
    }

    return primary;
}

} // namespace fujinet::core
```

This keeps **transport choice centralized and explicit**.

---

## Platform-Specific Channel Factories

### POSIX

```cpp
namespace fujinet::platform::posix {
    std::unique_ptr<fujinet::io::Channel> create_default_channel();
}
```

May use PTYs, sockets, stdin/stdout, etc.

### ESP32 / ESP-IDF

```cpp
namespace fujinet::platform::esp32 {
    std::unique_ptr<fujinet::io::Channel> create_default_channel();
}
```

May use UART, USB CDC, GPIO, SIO bit-banging, etc.

Only these platform files may see:
- ESP-IDF headers
- POSIX syscalls
- Pinmap macros

---

## Using the Core

POSIX:

```cpp
auto profile = config::current_build_profile();
auto channel = platform::posix::create_default_channel();
core::setup_transports(core, *channel, profile);
```

ESP32:

```cpp
auto profile = config::current_build_profile();
auto channel = platform::esp32::create_default_channel();
core::setup_transports(core, *channel, profile);
```

The shape is identical across platforms.

---

## Where `#ifdef` Is Allowed

✅ Allowed:
- `build_profile.cpp`
- platform channel factories
- thin platform bootstrap code

❌ Not allowed:
- Virtual devices
- Transports
- IOService
- RoutingManager
- IODeviceManager

---

## Benefits

- Build flags are centralized
- Core logic is platform-agnostic
- Far fewer `#ifdef`s
- Multiple transports per build are easy
- POSIX and ESP32 share the same architecture

---

## Adding a New Machine or Bus

1. Add enum entries
2. Update `build_profile.cpp`
3. Extend `setup_transports()`
4. Add a channel factory if required

No core surgery required.

## Naming Recap

- Channel: PTY vs UART vs USB vs socket (implementation detail)
- TransportKind: what protocol/framing we’re using on that channel (debug line vs FujiSlip vs SIO)
- ITransport implementation (Rs232Transport, later SlipTransport, etc.): maps bytes to IORequest/IOResponse
- BuildProfile + FN_BUILD_*: chooses TransportKind and Machine

## Clearing up the mental model (PTY vs RS232 vs SLIP)

Right now we have three concepts:

1. Channel (io::Channel)
“How do raw bytes move?”
Examples: PTY, UART0, USB CDC, TCP socket

2. Transport (ITransport, e.g. Rs232Transport)
“How do I frame & interpret those bytes as IORequests / IOResponses?”

e.g.: simple “newline-delimited” frames

Later: FujiNet SLIP + headers

3. BuildProfile::TransportKind

“What kind of protocol/link does this build expect from the host?”
This should really be about the protocol, not about “it’s a PTY”.

So:

- PTY is a type of Channel, not a protocol. It’s just “a pipe with a TTY personality”.
- Our current protocol is “line-based debug over serial-like link”.
- Later protocol will be “FujiNet SLIP framing over serial-like link”.