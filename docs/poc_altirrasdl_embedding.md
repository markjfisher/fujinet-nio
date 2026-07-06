# AltirraSDL Embedded FujiNet-NIO POC

*Status: proof of concept in progress*
*Created: 2026-07-06*

## Goal

Investigate embedding FujiNet-NIO directly inside AltirraSDL as a native SIO
bus device, instead of running:

```text
AltirraSDL custom atdevice -> netsiohub -> UDP -> external fujinet-nio process
```

The desired embedded shape is:

```text
AltirraSDL SIO manager
  -> native ATDeviceFujiNetNIO
  -> in-memory FujiBus Channel
  -> FujiBusTransport
  -> FujinetCore
  -> FujiNet virtual devices
```

This would make AltirraSDL a direct host application for the fujinet-nio
library, similar to the existing `tests/test_embed_core.cpp` embeddability
test.

## Repository Location

AltirraSDL fork:

```text
git@github.com:markjfisher/AltirraSDL.git
```

Workspace clone:

```text
repos/AltirraSDL
```

The workspace tracks it as a submodule so the POC can be developed alongside
`repos/fujinet-nio`, `repos/fujinet-nio-lib`, and the Atari app/test tooling.

## Initial Feasibility Checks

The following checks were run from the workspace.

### AltirraSDL configure

```shell
cd repos/AltirraSDL
cmake --preset linux-debug
```

Result:

- configure completed successfully;
- SDL3 and SDL3_image were fetched/configured by AltirraSDL's normal CMake
  flow;
- build files were generated under:

```text
repos/AltirraSDL/build/linux-debug
```

### FujiNet-NIO library-only build

```shell
cd repos/fujinet-nio
cmake -S . -B build/altirrasdl-embed-lib-poc \
  -DFN_BUILD_POSIX_APP=OFF \
  -DFN_BUILD_TESTS=OFF \
  -DFN_WITH_CURL=OFF \
  -DFN_WITH_OPENSSL=OFF \
  -DFN_BUILD_FUJIBUS_PTY=ON

cmake --build build/altirrasdl-embed-lib-poc --target fujinet-nio
```

Result:

- `libfujinet-nio.a` built successfully;
- the build did not require the POSIX app target;
- curl/OpenSSL can be disabled for a first embedded proof;
- yaml-cpp is still part of the current fujinet-nio library build because
  config loading uses `FujiConfigYamlStore`.

This is enough to justify a first integration spike.

## Relevant AltirraSDL Device Model

Altirra devices are registered through `ATDeviceDefinition` entries and device
factory functions.

Examples:

- `src/ATDevices/source/sioclock.cpp`
- `src/ATDevices/source/testdevicesiopoll.cpp`
- `src/ATDevices/source/testdevicesiohs.cpp`
- `src/Altirra/source/sio2sd.cpp`
- `src/Altirra/source/pclink.cpp`

Important Altirra interfaces:

```cpp
class IATDeviceSIO {
public:
    virtual void InitSIO(IATDeviceSIOManager *mgr) = 0;
    virtual CmdResponse OnSerialBeginCommand(const ATDeviceSIOCommand& cmd) = 0;
    virtual void OnSerialAbortCommand() = 0;
    virtual void OnSerialReceiveComplete(uint32 id, const void *data, uint32 len, bool checksumOK) = 0;
    virtual void OnSerialFence(uint32 id) = 0;
    virtual CmdResponse OnSerialAccelCommand(const ATDeviceSIORequest& request) = 0;
};
```

The command-level SIO API is probably enough for the first FujiNet-NIO POC.
Raw SIO (`IATDeviceRawSIO`) should be avoided unless we later need exact
bit-level timing or external clock behavior.

## Proposed Native Device

Create a new Altirra device:

```cpp
class ATDeviceFujiNetNIO final
    : public ATDevice
    , public IATDeviceSIO
{
    // Altirra device plumbing
    IATDeviceSIOManager* mpSIOMgr = nullptr;
    vdrefptr<IATDeviceSIOInterface> mpSIOInterface;

    // FujiNet-NIO embedded core
    fujinet::core::FujinetCore mCore;
    AltirraFujiBusChannel mChannel;
    fujinet::io::ITransport* mpTransport = nullptr;
};
```

The companion `AltirraFujiBusChannel` would be an in-memory implementation of
`fujinet::io::Channel`:

```text
Atari SIO write data -> channel RX queue -> FujiBusTransport
FujiBusTransport response -> channel TX queue -> Atari SIO read data
```

This is the same basic pattern as `tests/test_embed_core.cpp`, except the host
application is AltirraSDL rather than a doctest.

## SIO Command Mapping

The new Atari NIO FujiBus client sends FujiBus bytes over Atari SIO using
write/read command framing. The embedded Altirra device should implement that
same channel behavior directly.

Expected command shape:

```text
SIO W command:
  AUX = byte count
  receive FujiBus request bytes from Atari
  push bytes into embedded Channel RX
  run FujinetCore until response is available or command completes

SIO R command:
  AUX = requested response byte count
  read pending FujiBus response bytes from embedded Channel TX
  send complete + data + checksum through Altirra SIO manager
```

Command-level Altirra calls should be enough:

- `BeginCommand()`
- `SendACK()`
- `ReceiveData(id, len, true)`
- `SendComplete()`
- `SendData(data, len, true)`
- `EndCommand()`

The closest Altirra examples are:

- `sio2sd.cpp` for simple SIO read/write command handling;
- `pclink.cpp` for multi-phase receive/fence handling.

## Core Pumping Strategy

FujiNet-NIO is cooperative. The embedding application owns the main loop and
calls:

```cpp
core.tick();
```

For Altirra, there are two possible pump strategies.

### Phase 1: bounded synchronous pump

For simple fdrive/config/slot commands:

```text
receive SIO W data
push bytes into Channel RX
run core.tick() a bounded number of times
collect response bytes from Channel TX
serve following SIO R command
```

This should be enough to prove the direct embedding model.

The loop must be bounded. A bad or slow FujiNet operation must not hang the
emulator.

### Phase 2: scheduler-driven async pump

For slower operations such as TNFS, HTTP, DNS, or long network activity, the
device should avoid doing all work in the SIO callback. Instead it should use
Altirra's scheduler/fence/deferred-event model to continue ticking the embedded
core over emulator time.

This preserves emulator responsiveness and avoids turning SIO callbacks into
blocking network calls.

## Registration Work Needed In AltirraSDL

A first native device would likely need:

1. New device source, probably near the other SIO devices:

```text
src/ATDevices/source/fujinetnio.cpp
```

or, if it depends heavily on Altirra core services:

```text
src/Altirra/source/fujinetnio.cpp
```

2. Device definition:

```cpp
extern const ATDeviceDefinition g_ATDeviceDefFujiNetNIO = {
    "fujinetnio",
    nullptr,
    L"FujiNet-NIO",
    ATCreateDeviceFujiNetNIO
};
```

3. Register it in:

```text
src/Altirra/source/devices.cpp
```

4. Add it to the SDL UI catalog:

```text
src/AltirraSDL/source/ui/sysconfig/ui_system_pages_b.cpp
```

Likely category:

```text
SIO bus devices
```

5. Link `fujinet-nio` and expose include directories through CMake.

## Implemented First Slice

Implemented in the `feature/ATDeviceFujiNetNIO` AltirraSDL branch.

Files:

```text
repos/AltirraSDL/src/AltirraSDL/source/devices/fujinetnio.cpp
repos/AltirraSDL/src/AltirraSDL/CMakeLists.txt
repos/AltirraSDL/src/Altirra/source/devices.cpp
repos/AltirraSDL/src/AltirraSDL/source/ui/sysconfig/ui_system_pages_b.cpp
```

Current behavior:

- Adds an `ALTIRRA_FUJINET_NIO` CMake option.
- Auto-enables the option only when a sibling `../fujinet-nio` source tree is
  present, which matches the workspace POC layout.
- Builds `fujinet-nio` as a CMake subdirectory with the POSIX app/tests
  disabled.
- Enables `FN_BUILD_EMBEDDED_LIB` so the AltirraSDL build links a reduced POSIX
  library surface and does not pull in the standalone POSIX app, console
  transports, serial/PTY/TCP channel implementations, or NetSIO bridge channel
  glue.
- Keeps curl/OpenSSL enabled for the embedded build because FujiNet network
  operations are part of the expected device behavior.
- Registers a native `fujinetnio` Altirra device in the main device registry.
- Shows `FujiNet-NIO` under the SDL Configure System SIO bus device catalog.
- Exposes a `configdir` device property in the SDL configuration UI and through
  `--adddevice fujinetnio,configdir=...`.
- Implements a command-level SIO device that accepts the NIO FujiBus wrapper
  `W` and `R` commands on the same SIO IDs as the NetSIO bridge path:
  - `0x7F` for the NIO wrapper device;
  - `0x71` for the bridge network alias.
- Uses an in-memory `fujinet::io::Channel` to pass bytes directly between
  Altirra's SIO manager and `FujiBusTransport`.
- Boots a real `FujinetCore`, creates the Fuji device, registers the file,
  clock, disk, network, and modem devices, and applies configured disk mounts.
- Runs the embedded core from a worker thread. SIO write commands enqueue
  FujiBus request bytes and signal the worker; SIO read commands use Altirra
  fences and short retry delays until a response is ready. This avoids running
  FujiNet file/network operations on Altirra's emulator/UI path.

Validation run:

```shell
cd repos/AltirraSDL
cmake --preset linux-debug
cmake --build build/linux-debug --target AltirraSDL -j2
```

Result:

- configure succeeded;
- `fujinet-nio` built as part of the AltirraSDL target;
- `source/devices/fujinetnio.cpp` compiled and linked;
- `AltirraSDL` linked successfully.

This proves the source-level integration and native device registration are
feasible. It does not yet prove that `fdrive.xex` succeeds through the embedded
device at runtime.

## Embedded Runner Profile

The workspace runner has a dedicated profile for the native device:

```text
configs/atari/profiles/altirra-embedded-fujinet-nio.yaml
```

This profile uses:

```yaml
altirra:
  embedded_fujinet_nio: true
  devices:
    - type: fujinetnio
      params:
        configdir: ${ATARI_FUJINET_CONFIG_DIR}
```

The `scripts/build.sh atari-run` wrapper detects that flag and changes the
runtime shape:

- uses the workspace-built AltirraSDL binary at
  `repos/AltirraSDL/build/linux-debug/src/AltirraSDL/AltirraSDL`;
- creates the normal temporary `fujinet-data/fujinet.yaml` test config;
- passes that temporary data directory as the device `configdir`;
- does not start `netsiohub`;
- does not start an external `fujinet-nio` process;
- logs AltirraSDL/fujinet-nio output to `build/logs/atari-run.log`.

Command:

```shell
scripts/build.sh atari-run altirra \
  --profile configs/atari/profiles/altirra-embedded-fujinet-nio.yaml
```

Smoke result on 2026-07-06:

- AltirraSDL started with `--adddevice fujinetnio`.
- The embedded FujiNet-NIO core loaded `fujinet.yaml` from the temporary host
  filesystem.
- The fdrive test config applied three mounts.
- `fdrive.xex` issued FujiBus requests through the embedded native device.
- Responses included the expected configured slot URIs:
  - `host:/disks/slot0-fdrive-test.atr`
  - `host:/disks/slot1-readwrite-test.atr`
  - `tnfs://fujinet.online/atari/fujinet-nio-slot2.atr`

## Build Integration Options

### Option A: AltirraSDL pulls fujinet-nio as a subdirectory

This is the simplest source-level POC:

```cmake
set(FN_BUILD_POSIX_APP OFF)
set(FN_BUILD_TESTS OFF)
set(FN_WITH_CURL OFF)
set(FN_WITH_OPENSSL OFF)
set(FN_BUILD_FUJIBUS_PTY ON)
add_subdirectory(../fujinet-nio fujinet-nio-build)
target_link_libraries(AltirraSDL PRIVATE fujinet-nio)
```

Pros:

- fastest proof;
- uses existing fujinet-nio CMake target;
- no install/package step required.

Cons:

- currently drags in yaml-cpp;
- fujinet-nio POSIX build includes more platform sources than a minimal embedded
  library really needs;
- option names and generated CMake source lists are owned by fujinet-nio.

### Option B: fujinet-nio provides an explicit embed target

Longer term, fujinet-nio should probably expose a smaller target, for example:

```text
fujinet-nio-core
fujinet-nio-posix-services
fujinet-nio-embed
```

The Altirra device would link only what it needs:

- core;
- FujiBus transport;
- file/disk/config devices;
- selected filesystems/protocol backends;
- no POSIX app, console, NetSIO bridge channel, or emulator runner glue.

This would reduce link size and make dependencies clearer.

## Configuration Questions

The native Altirra device needs a clear config source.

Possible approaches:

1. Use a default `fujinet.yaml` path relative to Altirra's profile/config dir.
2. Add an Altirra device config property pointing to a fujinet config directory.
3. Start with an internal temporary config for POC, then expose UI later.

For the first proof, a fixed local config path is acceptable as long as it is
not hardcoded permanently.

## Licensing Note

FujiNet-NIO is GPLv3. AltirraSDL sources appear to carry GPLv2-or-later
headers. Before distributing a combined binary, verify license compatibility
for the exact fork/source set. For a private POC this is not a blocker, but it
should be resolved before upstreaming or packaging.

## Main Risks

- **Blocking network operations:** fujinet-nio operations must not freeze the
  emulator thread.
- **Bootstrap duplication:** `main_posix.cpp` currently wires config, storage,
  devices, filesystems, and services. The POC should avoid copying that logic
  into Altirra permanently. A reusable embedded bootstrap helper may be needed.
- **Dependency size:** yaml-cpp, optional curl, and optional OpenSSL need a
  deliberate embed story.
- **Device timing:** command-level SIO should be enough for NIO FujiBus W/R,
  but raw SIO may be needed later if software bypasses normal SIO paths.
- **Config persistence:** Altirra device config and FujiNet config need a clean
  boundary.

## Recommended First Spike

1. Add an `AltirraFujiBusChannel` in AltirraSDL that implements
   `fujinet::io::Channel` with RX/TX queues.
2. Add `ATDeviceFujiNetNIO` implementing `IATDeviceSIO`.
3. Build a minimal embedded FujiNet core:
   - create `FujinetCore`;
   - install `FujiBusTransport` over `AltirraFujiBusChannel`;
   - register only the devices needed for `fdrive.xex` first.
4. Implement SIO `W` and `R` command handling.
5. Run `fdrive.xex` directly in AltirraSDL and compare slot output with the
   current external NetSIO/bridge setup.

The proof is successful when `fdrive.xex` can query configured slots through
the embedded FujiNet-NIO device without starting:

```text
netsiohub
external fujinet-nio process
Altirra custom .atdevice bridge
```
