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

Run these scripts from this directory, or invoke them by path from anywhere.
They locate the project and source the workspace environment automatically when
available. You do not need to export `NIO_WORKSPACE` or assemble a CMake recipe.

| Command | Purpose |
| --- | --- |
| `./scripts/setup.sh` | Prepare all pinned sources and check build prerequisites. |
| `./scripts/setup.sh --install-toolchain` | Also install the verified local Arm compiler if needed. |
| `./scripts/setup.sh --repair` | Preserve and replace altered local dependency checkouts. |
| `./scripts/setup.sh --host-only` | Prepare native tests only; no SDK or Arm compiler. |
| `./scripts/create-deps.sh` | Fetch/validate source dependencies only, without compiler setup. |
| `./scripts/create-deps.sh --check` | Check dependencies without downloading or modifying them. |
| `./scripts/test.sh` | Configure, build and test native Debug and Release. |
| `./scripts/build.sh firmware` | Build the Core2350B capture fixture. |
| `./tests/feasibility/generator-check/run.sh build` | Build/test the RP2040 generator and USB loader. |

Each script has `--help`. Setup is idempotent: clean pinned dependencies are
reused. Build scripts perform their required source setup too. No command here
loads a board or generates signals.

Prerequisites: Git, Python 3, CMake >=3.21, Ninja and a native C/C++ compiler.
Firmware needs an Arm embedded toolchain; generator USB loading also needs
`pkg-config` and libusb development files, and capture uses `sigrok-cli` with
fx2lafw firmware. Setup reports missing system tools; it does not install system
packages or run sudo. The optional compiler installer verifies the pinned
Linux x86_64 Arm GNU 14.2.Rel1 archive before extracting it locally.
That optional installer needs Python 3.12+ or a security update providing
`tarfile.data_filter` for safe extraction.

### Recovering accidentally formatted dependencies

Run `./scripts/setup.sh --repair`, then rerun your original build or experiment.
Repair preserves invalid managed checkouts under `.deps-backups/`, prints their
locations, and restores the source pins. It includes required SDK submodules;
it leaves clean dependencies alone. Backups remain available if restoration
fails. Normal setup/build never silently discards edits.

`.deps/` contains third-party source, not project code. The bridge's
`.clang-format-ignore` excludes dependencies, backups and build products from
file-based clang-format runs. Editor integrations or other formatters may need
matching exclusions of their own.

### Existing SDK and compiler installations

`PICO_SDK_PATH` selects an existing SDK; it must match the exact pin and required
submodules. An external SDK is validated only, even with `--repair`. Unset that
override to use the managed `.deps/pico-sdk`. Scripted firmware configuration
passes the selected SDK explicitly so a stale CMake cache does not override it.
`PICO_TOOLCHAIN_PATH` selects an existing Arm compiler directory. Setup also
recognizes an installed compiler and the workspace's local toolchain cache.

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
`scripts/bootstrap.py` remains the low-level fetch/validation implementation used
by CMake. Use the script entry points above for normal setup and recovery. Changing
a dependency version is a separate deliberate edit to `dependencies.json`, followed
by repair and validation; repair itself never changes pins.
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
exit for forbidden source/build rules. The bridge is outside the main firmware
source collector. The product's existing
generated source-list drift is unrelated to this independent project; bridge
setup never regenerates those product lists.

## Next: Story 2.2 physical feasibility

For a different computer or RP2040, follow [bench setup and portability](docs/bench-setup.md).
USB mode IDs are shared across compatible boards; the selected board identity
belongs in ignored local configuration, not the experiment manifest.

The [experiment plan](docs/story-2-2-experiment-plan.md) extends this skeleton with
an independent RP2040 stimulus generator and USB-observable Core2350B DUT, then
measured timing and later buffered real-bus validation. C0 now has a RAM-only
RP2350 capture-counter firmware and a combined two-board runner; later stimulus
and real-bus cases remain planned. The isolated [RP2040 W0 generator](docs/rp2040-generator.md)
uses the `stimulus-rp2040` preset with explicit USB run/stop control.

Start repeatable bench work with the [experiment index](tests/feasibility/README.md)
and `tests/feasibility/generator-check/run.sh`. Inspect, build, load, run and
analyse independently; the default flow guides you through BOOTSEL and waits
before output. C0 is implemented pending its physical evidence run; C1–C10 remain
explicit planned cases.
