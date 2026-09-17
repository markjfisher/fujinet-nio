# RP2350 Zorro bridge skeleton

This independent project provides a synthetic input capture fixture for RP2350B
(`waveshare_core2350b`, `rp2350-arm-s`). It does not implement Zorro-II transactions,
a packet ABI, an ESP link, DMA, or a FujiNet service stack. It is not a claim of
working hardware. The production firmware root build does not include this tree.

The same `src/capture_program.c` uses apio C macros for firmware and native epio
behavior tests. First-party PIO text sources and assembler generation are forbidden.
Only native tests define `APIO_EMULATION`; epio is compiled from source with
`-fshort-enums`, propagated to consumers to match its public structure ABI.

## Reproducible setup

Prerequisites: Git, Python 3, CMake >=3.21, Ninja, and a native C compiler.
Firmware additionally needs a native C++ compiler and an Arm embedded toolchain.
Verified: native GCC 16.2.1 and Arm GNU 14.2.Rel1 (GCC 14.2.1 20241119).
An installed `arm-none-eabi-gcc` can be used, or install the verified Linux x86_64
compiler locally, without root:

```sh
export NIO_WORKSPACE=/path/to/fujinet-nio-workspace
source "$NIO_WORKSPACE/scripts/env.sh"
curl -fL -o /tmp/arm-gnu-toolchain-14.2.rel1.tar.xz \
  https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz
printf '%s  %s\n' 62a63b981fe391a9cbad7ef51b17e49aeaa3e7b0d029b36ca1e9c3b2a9b78823 \
  /tmp/arm-gnu-toolchain-14.2.rel1.tar.xz | sha256sum -c -
mkdir -p "$NIO_WORKSPACE/build/toolchains"
tar -xJf /tmp/arm-gnu-toolchain-14.2.rel1.tar.xz -C "$NIO_WORKSPACE/build/toolchains"
export PICO_TOOLCHAIN_PATH="$NIO_WORKSPACE/build/toolchains/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin"
```

From this directory, native setup never needs an SDK, ARM compiler, ESP-IDF,
reference checkout or prebuilt emulator archive:

```sh
source "$NIO_WORKSPACE/scripts/env.sh"
python3 scripts/bootstrap.py --mode host
python3 scripts/bootstrap.py --mode host
cmake --preset host
cmake --build --preset host
ctest --preset host
cmake --preset host-release
cmake --build --preset host-release
ctest --preset host-release
python3 scripts/check_pio_policy.py
```

Firmware setup does not require epio or the host build. By default it clones the
SDK independently into `.deps/pico-sdk`. If `PICO_SDK_PATH` is set, bootstrap and
CMake validate that checkout instead: it must be at the exact pin, clean, and have
the required submodules initialized. A wrong/missing override is rejected.
Use `unset PICO_SDK_PATH` to return bootstrap to the local default. CMake caches
its SDK selection: also remove the firmware build directory or explicitly reset
it with `cmake --preset firmware -DPICO_SDK_PATH="$PWD/.deps/pico-sdk"`. Unsetting
the environment alone does not replace a cached `-DPICO_SDK_PATH` override.

```sh
source "$NIO_WORKSPACE/scripts/env.sh"
export PICO_TOOLCHAIN_PATH="$NIO_WORKSPACE/build/toolchains/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin"
python3 scripts/bootstrap.py --mode firmware
cmake --preset firmware
cmake --build --preset firmware
```

Outputs: `build/firmware/bridge_capture.elf` and `bridge_capture.uf2`.
SDK import precedes `project()`; SDK initialization follows it. Wrong board or
platform selections fail configuration. Source pins in `dependencies.json` are:

| Dependency | Release | Commit |
| --- | --- | --- |
| apio | v0.3.0 | `1023d866849694417ced497e17e98a2cd2bd026e` |
| epio | v0.2.1 | `bf49b19a534befb69bd518ee463365a1a5d73a65` |
| Pico SDK | 2.3.1 | `079c6f39023649b154152db30f1d781e884879bc` |
| picotool (UF2 host tool) | 2.3.1 | `2041936441b48a3cc53ae3da9e805229fe8f4e18` |

SDK `lib/tinyusb` is initialized recursively at its SDK gitlink pin; wireless
libraries are not required by this board. Picotool is built from the validated
source checkout rather than selected from the host or an unpinned fetch.
Bootstrap is idempotent and never resets dirty or stale existing source trees.
To refresh dependencies intentionally, preserve local work, update the manifest's
release and full revision together, remove the affected `.deps` checkout and
`build` configurations, bootstrap again, and rerun both verification paths.
Caches, binaries and generated files are ignored. Configure and every native or
firmware build validate clean pinned dependency sources and the first-party PIO
policy before compiling their consumers. Manifest edits trigger reconfiguration.
Both CTest presets treat an empty test selection as an error.

## Fixture and evidence boundaries

Synthetic GPIO1 is an active-low strobe with a pull-up. GPIO2–5 are four input
bits with pull-downs. PIO0 SM0 waits for low, samples, pushes a full FIFO word,
sets IRQ0, then waits for high before wrapping. There are no output GPIOs, stdio
is disabled (UART overlaps the fixture), and firmware polls/drains FIFO and IRQ;
no hardware IRQ handler is installed. Normal hardware input synchronization is
preserved. Never connect this provisional fixture directly to a Zorro bus.

Native tests use independent literal GPIO levels, complete 32-bit expected words
`0x0000000a` and `0x00000005`, IRQ checks and exact successive instruction cycles.
They check idle, capture, sampling before PUSH, held-low suppression, and release/
rearm. Their checks remain active under `NDEBUG` in Release.

Epio does not model the hardware input synchronizer delay. Its cycle assertions
establish instruction behavior in the emulator, not external bus timing. Physical
synchronization, electrical voltage/timing, DMA/peripheral interactions, reset,
and bridge-link operation require instrumented validation in later stories.

The tooling tests use temporary local Git origins to test bootstrap repeatability,
wrong pins, dirty sources and missing dependencies; policy fixtures verify nonzero
exit for forbidden source/build rules. Existing source collection can be checked
from the firmware repository with:

```sh
./scripts/update_cmake_sources.py
git diff --exit-code -- CMakeLists_posix.cmake src/CMakeLists.txt
```

At firmware baseline `2365fbce15391ae445f38962909ab8c8f172d84e`, this generator already changes profile selection
in both checked-in lists. Story 2.1 verified byte-identical generated outputs
with and without the bridge tree, then preserved the existing checked-in lists.
Until that separate generator drift is repaired, run it in a disposable copy
or save and restore these two files; its nonzero diff is not bridge inclusion.

## Next: Story 2.2 physical feasibility

The [experiment plan](docs/story-2-2-experiment-plan.md) extends this skeleton with
an independent RP2040 stimulus generator and USB-observable Core2350B DUT, then
measured timing and later buffered real-bus validation. It defines staged wiring,
test cases, implementation packages and evidence requirements. Its new firmware
targets and run commands are planned, not available in the Story 2.1 skeleton.
