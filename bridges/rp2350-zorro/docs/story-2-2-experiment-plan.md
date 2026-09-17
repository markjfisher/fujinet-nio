# Story 2.2 — RP2350B Zorro-facing feasibility experiment

Status: staged plan, updated 2026-09-17. The generator-first slice now has
an isolated RAM-only RP2040 target, shared APIO instruction tests, USB control
and initial independent analyzer captures; see the [generator guide](rp2040-generator.md)
and [measured evidence](feasibility/results/2026-09-17-generator/report.md).
This partially delivers E0–E2; the DUT observer and two-board runner are not
implemented. Work-package checkboxes remain open until their full gates pass.
Commands/targets in the original planned interface below remain future work
unless explicitly documented as available in the generator guide. This remains
one Story 2.2, not new dispatch stories, and is not accepted Zorro feasibility.

## Goal and relationship to Story 2.1

Determine which timing-sensitive bus-facing operations an RP2350B can perform,
under what conditions, and with what measured margins. Start with independent
3.3 V synthetic stimulus, then validate representative real-bus conditions when
the passive Zorro-II breakout and A500/Zorro-II adapter are available.

Story 2.1 supplies [the isolated project](../README.md), pinned Pico SDK/apio/epio,
separate host/firmware builds, shared APIO capture code, and enforced PIO policy.
Its firmware currently discards captured words and has USB stdio disabled. This
story adds observable DUT firmware, independent RP2040 stimulus, a repeatable
experiment runner, more PIO behaviors, external traces and a feasibility report.

All work remains under `bridges/rp2350-zorro/`, except workspace task wrappers.
No FujiBus/service integration, ESP32-S3 test project, RP2350-to-ESP protocol,
production register/mailbox ABI, or production pin/routing redesign. The RP2040
program is laboratory equipment. Story 2.3 owns bridge-to-ESP feasibility.

## Apparatus and readiness

| Item | Availability / role |
| --- | --- |
| Waveshare Core2350B | Available, headers fitted, USB working; device under test (DUT) |
| RP2040 development board | Available: TZT Pico-style purple AliExpress board, advertised 16 MB flash, USB-C, BOOTSEL, 40 pins; independent stimulus generator |
| Breadboard and Dupont leads | Available; short signal paths and common GND for initial 3.3 V experiments |
| Two USB data connections | Required, one console per board; identify each by reported role and unique board ID |
| Logic analyzer | Available: inexpensive eight-channel USB unit labelled CH1–CH8 plus GNC/CLK (ground label as reported); exact model, USB connector, software, input limits and sample-rate capabilities unverified |
| Oscilloscope and suitable probes | HANMATEK DOS1102 available; verify probe configuration and usable measurement capabilities. Use for timing, analog edge quality and output release |
| Multimeter | Simple unit available; use for continuity and supply/ground checks |
| Weak bias / series resistors, later buffers | Select and document with the output/release fixture; needed before bidirectional tests, not guessed production parts |
| Passive Zorro-II breakout | Being fabricated; provides access only, no level conversion or output protection |
| A500/Zorro-II adapter and actual host | Later real-bus stage; confirm exact board/revision, connection and availability |

The user identifies a TZT Pico-style RP2040 clone with a separate four-pin
debug/power adapter. Verify its exact header map and flash device/configuration
before wiring/flashing; do not assume the official Pico SDK board configuration
fully describes the advertised 16 MB clone. The debug header is not required
for this USB bench. GPIO names below are chip GPIO numbers, not physical header positions. The runner must reject
an unverified board/wiring profile. Board identification and instrumentation gaps
do not prevent host tests, target plumbing or the portability spike.

Both boards are independently USB-powered for the bench; connect GND and signal
nets, not their 3V3/VBUS/5V supply outputs. Keep this stage at 3.3 V. Disconnect
signals when a board is unpowered unless the fixture explicitly provides isolation.
Disable UART stdio, onboard functions conflicting with fixture pins, and stimulus
outputs until explicitly armed. USB enumeration never starts a waveform.

Select analyzer sample rate/channel count and scope bandwidth from the shortest
interval and edge being measured, at the simultaneous channel count in use.
Record sampling uncertainty and probe loading. A slow analyzer is useful for
bring-up but cannot certify a tighter limit than it resolves. USB consoles are
sufficient for initial pattern/count/recovery diagnostics. They
do not measure pin timing or prove output release. Without external measurements,
record functional results and **timing unverified**, not inferred nanosecond margins.

## Wiring profiles

These are proposed synthetic bench assignments, not a Zorro pinout. Before first
use, record both board models/revisions, map each GPIO to its actual header label,
check continuity, and retain a wiring photo. Do not mix profiles across binaries.

### W0 — reproduce the existing four-bit capture

User reports wiring W0 in progress (2026-09-17); completion and continuity checks
are not yet recorded. Wire with both USB supplies disconnected. Use the multimeter
to check the GPIO-to-GPIO connections and unintended shorts before powering.
RP2040 stimulus/USB diagnostics are now available; RP2350 observer firmware
remains to be implemented. Analyzer observations verify the generator signals,
not a completed electrical continuity or DUT test record.

| Net | RP2040 generator | Core2350B DUT | Initial state / ownership |
| --- | --- | --- | --- |
| GND | GND | GND | Common reference |
| D0 | GP2 output | GP2 input | Generator drives data |
| D1 | GP3 output | GP3 input | Generator drives data |
| D2 | GP4 output | GP4 input | Generator drives data |
| D3 | GP5 output | GP5 input | Generator drives data |
| /AS | GP6 output | GP1 input | Deasserted high before arming |
| USB | Generator USB | DUT USB | Separate host ports; no inter-board USB link |

User-reported W0 analyzer signal wiring (2026-09-17):

| Analyzer channel | Signal | Core2350B GPIO |
| --- | --- | --- |
| CH1 | D0 | GP2 |
| CH2 | D1 | GP3 |
| CH3 | D2 | GP4 |
| CH4 | D3 | GP5 |
| CH8 | /AS | GP1 |

The unit is labelled CH1–CH8 plus GNC/CLK as reported by the user. CH5–CH7
are unused. Confirm the reported GNC terminal is ground, then connect it to the
boards' common GND; ground connection and input compatibility are not yet verified.
Leave CLK disconnected until its function/pinout is identified. Capture software
must map its channel numbering explicitly to these physical labels, including
CH8 for /AS. Five simultaneous channels cover W0; eight channels cannot observe
the entire sixteen-bit W1 fixture at once. Later captures must name the observed
subset; separate runs are not a simultaneous full-bus trace. Signal attachment
alone does not establish completed continuity checks or a passing experiment.

This preserves Story 2.1's DUT GP1 /AS and GP2–5 data configuration. No DUT output
or acknowledgement wire is needed for the initial capture experiment. Analyzer
channels observe the actual /AS and data nets; an internal PIO IRQ is not a pin.
An optional external timing marker must be separately mapped and its latency
measured; a CPU-toggled marker is not proof of the exact capture instant.

### W1 — candidate wider, bidirectional fixture

Switch only after W0 passes. Rewire while disarmed; both firmware roles must
report W1. Confirm every pin is exposed and unreserved on the selected RP2040.

| Net | RP2040 GPIO | Core2350B GPIO | Ownership |
| --- | --- | --- | --- |
| D[15:0] | GP2–17 | GP2–17 | Generator for writes; DUT for reads, otherwise released |
| /AS | GP18 | GP18 | Generator -> DUT |
| R/W | GP19 | GP19 | Generator -> DUT; 1=read, 0=write in this fixture |
| /UDS-like | GP20 | GP20 | Generator -> DUT, active low |
| /LDS-like | GP21 | GP21 | Generator -> DUT, active low |
| SELECT | GP22 | GP22 | Generator -> DUT, active high synthetic selection |
| /ACK | GP26 input | GP26 output | DUT -> generator, active low provisional completion marker |
| GND | GND | GND | Common reference |

First use W1 with four active data bits, then eight, then sixteen. Output enable,
series resistance, bias and any monitor nets must be specified in the W1 fixture
record before enabling DUT outputs. Sequence writes and reads with an explicit
both-released interval. Do not deliberately create opposing push-pull outputs.
A high-impedance state must be checked using external bias/measurement, not just
by reading the DUT's output-enable register.

SELECT substitutes for address decode only in synthetic tests. This fixture does
not drive a full Zorro address bus. Record the eventual address/control/data pin
budget, PIO GPIO windows, instruction memory and state-machine usage; W1 passing
is not evidence that the final mapping or address decoder fits. Additional RP2040
boards may supply a separately validated fixture extension if needed; no ESP is
required. The real-bus wiring will have its own reviewed profile, not reuse W1
as a connector pinout.

## Firmware and test architecture

The generator executes finite, preloaded PIO waveform sequences independently of
the DUT clock. USB configures and retrieves results; it does not pace individual
edges. Monitor generator underrun, sequence completion and read-sampling capacity.
A starved generator or incomplete trace invalidates that run, rather than counting
as DUT success or failure. Use bounded buffers; prepare SRAM/DMA feeding only when
needed and validate its effect on the generated waveform.

The DUT initially compiles the existing `src/capture_program.c`. Add USB CDC
reporting in a separate feasibility executable, preserving the existing skeleton.
Collect data in bounded RAM with counters for received words, mismatches, overflow,
interrupt status and resets; defer verbose output until the timed burst ends.
Compare CPU polling/interrupt/DMA draining only as measured experiment variants,
not a permanent firmware architecture. Preserve input synchronization initially;
record any later change as a distinct experiment, never silently bypass it.

**APIO/RP2040 compatibility gate:** pinned apio v0.3.0 is RP2350-oriented and its
`include/apio_reg.h` has RP2350 reset/pad/PIO register addresses. It must not perform
hardware initialization on RP2040. Proposed lab implementation uses a shared C
stimulus builder with APIO instruction-encoding macros and an RP2040 Pico SDK
loader/configuration shim. The builder's emitted words and configuration manifest
are used by the native epio stimulus tests and by the RP2040 loader; no hand-copied
instruction arrays, `.pio` source or pioasm build step. Restrict instructions and
configurations to the RP2040-supported subset and verify them against the pinned
SDK/RP2040 documentation. Prove this approach in E0 before relying on it. If it
fails, report the exact incompatibility and revise the shim/design; do not quietly
weaken the APIO-only policy or switch to ESP32-S3. Epio is an RP2350 emulator, so
passing these shared-instruction tests does not establish RP2040 hardware timing.

For every representable DUT behavior, record a failing epio test first, then a
passing test against the same APIO source compiled for the DUT. Reuse the 2.1
red/green record for unchanged W0 behavior; new behaviors require new red/green.
For analog effects, synchronizer behavior or unsupported emulator features,
record the model gap and write the physical test/oracle before changing behavior.
Do not fake unsupported effects in order to report an epio pass.

The host runner computes expected values independently of the DUT implementation.
Use explicit `/dev/serial/by-id/...` ports, verify role/board ID/firmware hash/profile
on both consoles, reject swapped/duplicate ports and incompatible protocol versions,
then assign a fresh run ID. This USB command format is lab control, not FujiBus or
a future bridge-link ABI. Detect disconnects, malformed/truncated reports, timeout,
FIFO loss and log loss. On normal completion or host error, disarm outputs. Give
both boards local bounded-run/watchdog cleanup so host disappearance cannot leave
DUT outputs asserted indefinitely. Expected fault injection is recorded explicitly.

## Experiment matrix and order

All durations/counts below are proposed lab parameters, not Zorro timing limits.
First implement the named cases as deterministic tests and a physical run recipe.

| ID | Stimulus / fault | Independent expectation and evidence |
| --- | --- | --- |
| C0 idle | /AS high; change data | No captures or IRQ activity during the bounded observation |
| C1 patterns | All 16 four-bit values, alternating 0xA/0x5, then seeded sequences | Exact sequence/full words; one capture for each assertion, no silent discard |
| C2 held-active | Assert /AS once, hold it low while changing data | One sample only; held duration does not create another capture |
| C3 sampling window | Change data at swept offsets before/after assertion | Map old/new sample transition; meet expectations only outside the measured uncertainty window |
| C4 repetition | Repeated assertions, decreasing high gap and low width separately | Ordered exact counts above established limits; characterize failures below them |
| C5 width/control | 4 -> 8 -> 16 bits; walking bits, R/W, lane strobes, SELECT | Correct data grouping; no response when unselected; selected lanes follow the recorded fixture rule |
| C6 pressure | Pause/slow DUT drain until FIFO fills, then resume | Identify actual stall/loss behavior; no completion for unaccepted data; account for all losses/timeouts |
| C7 reads | Generator releases data; DUT returns a known pattern on selected reads | Generator samples expected data; measured data-valid and /ACK timing, including unavailable data |
| C8 turnaround | Alternate read/write and lane selection; vary release gap | Correct first value after each change; measured release, no simultaneous drive; unselected bus stays released |
| C9 recovery | Abort burst, soft-reset/reboot either endpoint, disconnect console | Outputs return to defined idle/released state; no old-run samples/replies; next armed run matches expected data |
| C10 real bus | Defined actual-host accesses through reviewed interface | Trace each applicable bus requirement to captured timing/electrical evidence and pass/fail/untested verdict |

C6 must preserve the baseline result even if it exposes a limitation: a blocking
PUSH prevents FIFO overwrite but does not guarantee that later strobes are observed.
First characterize the unchanged capture path, then test a separate provisional
handshake variant. Do not erase loss by slowing the generator implicitly. Likewise,
/ACK in C7 is a test signal; final /DTACK behavior and deadlines come from the
real-bus requirements, not from this fixture convention. No mailbox is needed.

Bring-up defaults: 100 microseconds low/high and generous data setup/hold; 256
assertions per basic case. Next halve one interval at a time until a failure or
instrument limit, then refine around the transition in generator-cycle increments.
Preserve known-good settings for the other variables. For chosen operating points,
propose 1,000,000 assertions and three independently rearmed runs, including a
reboot; report actual executed counts, not just the intended count. Tune run size
for bounded buffers and report the statistical scope of a zero-error result.

Independent oscillators help expose phase variation but do not guarantee exhaustive
phase coverage. Vary start phase, pulse/gap lengths and generator clock/divider;
measure the resulting coverage. Count checks alone establish functional reliability
under those conditions, not exact external timing. Keep breadboard/lead lengths,
voltage, clocks, silicon revisions and instrument settings with every result.

For a measured deadline, report `margin = allowed maximum - observed maximum -
measurement uncertainty`. For a minimum setup/hold requirement report the converse
minimum-bound comparison. Negative or unknown margin is not a pass. Distinguish
last passing/first failing stimulus settings from a characterized guarantee. Before
qualification, populate a bus-requirement ledger with source edition/page, signal,
min/max limit, applicable host conditions, measurement method and trace ID. Until
those values and adequate instruments exist, mark Zorro timing margin unverified.

## Incremental implementation breakdown

Paths below are relative to this bridge directory unless prefixed `workspace:`.
Each package ends in a reviewable result; they do not add entries to stories.yaml.

| Package | Files / action | Completion test / gate |
| --- | --- | --- |
| [ ] E0: portability and fixture definition | `tests/feasibility/test_stimulus_program.c`, `lab/rp2040/stimulus_program.{c,h}`, `lab/rp2040/pio_loader.c`, `docs/feasibility/wiring.md` — test APIO instruction builder/SDK loading and map actual boards; record instruments | Native encoding/configuration checks, RP2040 compile and slow pulse/capture bring-up; physical header mapping confirmed, external waveform verification required before timing claims |
| [ ] E1: integrated targets | Extend `CMakeLists.txt`, `CMakePresets.json`, `cmake/host.cmake`, `scripts/bootstrap.py`, `tests/test_tooling.py`; add `cmake/feasibility.cmake` — preserve 2.1 presets, isolate RP2040 platform/board validation and caches | Existing host Debug/Release, firmware and policy checks pass; new DUT and stimulus ELF/UF2 build; incorrect board/platform rejected |
| [ ] E2: USB observability and runner | `src/feasibility/main.c`, `src/feasibility/console.{c,h}`, `lab/rp2040/main.c`, `tests/feasibility/run_bench.py`, `tests/feasibility/test_runner.py` — implement role/ID handshake, bounded logging, ARM/RUN/STOP/results, timeout cleanup | Fake-console tests for swapped IDs, stale run IDs, loss/timeout; two real consoles enumerate; outputs remain inactive until armed |
| [ ] E3: physical four-bit reproduction | `tests/feasibility/cases/w0.json`, `tests/feasibility/test_capture_cases.c`; reuse `src/capture_program.c`; `docs/feasibility/results/` — execute C0–C4 | Exact data/count functional evidence, red/green for additions, external pulse/window traces where instruments permit; limits explicitly bounded |
| [ ] E4: width/control/pressure | `src/feasibility/bus_program.{c,h}`, `tests/feasibility/test_bus_program.c`, `tests/feasibility/cases/w1.json`; extend generator and W1 wiring — C5/C6 | Test-first selectable groups and explicit pressure outcome; pin/SM/instruction budget recorded; no false acknowledgements or unreported loss |
| [ ] E5: reads/turnaround/recovery | Extend E4 APIO source/tests/cases, DUT logging and RP2040 receiver — C7–C9 | Both-side expectations, external read-valid/release measurements and local timeout/reset cleanup; bidirectional fixture reviewed before outputs enabled |
| [ ] E6: real-bus procedure and execution | `docs/feasibility/zorro-requirements.md`, `docs/feasibility/real-bus-procedure.md`, applicable `src/feasibility/` probe and focused Amiga exerciser only if needed — C10 | Breakout/adapter, reviewed buffering/power/direction and instruments available; cited bus limits and actual captures; no uncontrolled host address writes |
| [ ] E7: evidence and verdict | `docs/feasibility/report.md`, `docs/feasibility/results/`; workspace scope/acceptance link — audit case/requirement coverage | Software/bench/real-bus outcomes separated; reproducible evidence, measured margins and explicit proceed/hold with unresolved items |

E0 and E1 may have a minimal build dependency on each other; implement just enough
E1 plumbing to compile the E0 spike, then finish E1 before E2. E3 physical capture
can begin before E4/E5 or the real-bus hardware arrives. Missing equipment blocks
only the experiments needing it. Passing E3 alone cannot mark Story 2.2 physically
accepted. Implementation remains incremental within the existing skeleton.

Add discoverable workspace wrappers in `workspace:tools/build/nio_build/tasks.py`
and argument routing in `workspace:tools/build/nio_build/cli.py`, with focused
coverage under `workspace:tools/build/tests/`. Proposed names: `rp2040-stimulus`
(build only), `rp2350-feasibility` (host checks plus both firmware builds), and
`rp2350-feasibility-run` (explicit two-port hardware execution). `--list` and
`--explain` must show the distinction. Existing `rp2350` tasks remain unchanged;
no normal build task silently flashes boards, opens consoles or drives pins.

## Planned build/run interface

Existing regression command from the workspace: `scripts/build.sh rp2350`.
The following commands are the implementation contract and **do not exist yet**.
After sourcing the workspace environment, use this bridge as the working directory:

```sh
python3 scripts/bootstrap.py --mode host
cmake --preset host
cmake --build --preset host
ctest --preset host
cmake --preset host-release
cmake --build --preset host-release
ctest --preset host-release

python3 scripts/bootstrap.py --mode firmware
cmake --preset dut-feasibility
cmake --build --preset dut-feasibility

python3 scripts/bootstrap.py --mode stimulus
cmake --preset stimulus-rp2040 -DPICO_BOARD="$RP2040_BOARD"
cmake --build --preset stimulus-rp2040

python3 tests/feasibility/run_bench.py \
  --dut-port "$DUT_PORT" --stimulus-port "$STIMULUS_PORT" \
  --wiring W0 --cases tests/feasibility/cases/w0.json \
  --output "$NIO_WORKSPACE/build/feasibility/run-001"
```

`RP2040_BOARD` is the verified SDK board ID, not guessed from the chip family.
Presets use separate `build/dut-feasibility` and `build/stimulus-rp2040` directories;
firmware targets are `feasibility_dut` and `feasibility_stimulus` respectively.
DUT retains `waveshare_core2350b` / `rp2350-arm-s`; stimulus uses `rp2040`, never
RP2350 APIO MMIO setup. Share the locked dependency sources/picotool without
sharing cross-compilation caches. Host tests remain SDK/ARM-independent.

The runner owns bounded port I/O and must document/install its host serial
prerequisite (for example a pinned pyserial dependency) before the command is
considered ready. Flash each role's UF2 via its board's documented USB bootloader
procedure, verify its returned identity, and only then arm the run. The exact
button/port and physical header instructions are part of the selected board profile.

For future implementation, run the narrow firmware-owner gates above plus
`python3 scripts/check_pio_policy.py` and the new runner tests. If workspace tools
change, run `PYTHONPATH=tools/build:tools python3 -m unittest discover -s
tools/build/tests -v` from the workspace after sourcing the environment. Library
or driver edits are not planned; any necessary later Amiga exerciser requires its
own named native/targeted build checks before implementation.

## Real-bus transition and acceptance

Before connecting the later breakout, identify the authoritative A500/adapter/
Zorro-II documents and preserve exact excerpts/limits in the requirement ledger.
Map the actual address decode, selection, strobes, data direction, response and
reset obligations; include configuration-chain requirements if the probe needs
to participate in enumeration. A passive breakout is not a transceiver.
Document voltage compatibility, power sequencing, high-impedance defaults and
buffer/OE delays. RP2350 input-voltage qualifications do not establish complete
interface suitability. Begin with passive observation through the approved input
path, then bounded accesses to a deliberately assigned test region using a
reviewed procedure. Do not invent a fixed host address or enable DUT bus outputs
before the electrical/selection procedure is validated. This is still a bus probe,
not FujiBus transport or a final register ABI.

Define these separate outcomes in the report:

- **software verified:** applicable epio/runner tests and both target builds pass;
- **bench functional verified:** independent RP2040 stimulus yields expected
  sequences, pressure/reset behavior and explicit exceptions on the physical DUT;
- **bench timing characterized:** external traces establish stated timing bounds
  and uncertainty on the documented fixture only;
- **real-bus proceed / hold:** representative A500/adapter captures meet the
  applicable cited obligations, or name the failures and untested obligations.

A positive Story 2.2 prerequisite for 2.4 requires the relevant real-bus evidence
and reviewed margins. If the breakout, interface, instruments or procedures are
missing, record partial progress and **hold** for that prerequisite. Do not replace
it with epio, same-chip loopback, or synthetic bench success. A hold is a valid
feasibility finding, not permission to approve the ABI. Story 2.4 separately still
requires accepted 1.6, positive relevant 2.3 evidence and explicit human approval.

## Evidence package

Each run must retain: full source revisions and dirty state; dependency/compiler
versions; ELF/UF2 hashes for both boards; reported role/board/silicon IDs; wiring
profile/header map/photo and lead lengths; supply/clock/divider/synchronizer settings;
case input/seed, intended and completed counts; expected and observed data; pressure,
underrun/overflow/reset/disconnect counters; result status; raw console transcripts;
analyzer/scope model/settings/channel map and raw trace files; measurement
uncertainty, calculated margins, failure boundary and untested cases.

Keep raw output in ignored `build/feasibility/<run-id>/`. Promote the reviewed
summary and manageable traces into `docs/feasibility/results/<run-id>/`; large
captures require a durable accessible artifact link plus hash, not a `/tmp` path.
A missing field/capture needed for a claim is an explicit evidence gap. Preserve
failed runs and explain exclusions; do not publish only the fastest passing run.

## Unresolved inputs and source notes

- TZT RP2040 clone exact revision, flash device/SDK configuration and physical
  header map; W0/W1 remain provisional until verified. The advertised memory size
  is user-reported, not a verified board specification.
- Eight-channel analyzer exact model, host capture software, input limits and
  simultaneous sample rate; HANMATEK DOS1102 probe configuration and usable
  measurement resolution. All three instruments are available, including the
  multimeter. Start with USB functional diagnostics and W0 traces; qualify each
  timing measurement against actual instrument settings.
- Breakout and A500 adapter revisions/schematics, buffering circuit and timing
  source editions; resolve before E6, not by importing a speculative production ABI.
- Verify the proposed APIO instruction-builder/RP2040-loader seam in E0; no claim
  of upstream RP2040 support is made.

Sources inspected: Story 2.1 files at firmware revision
`6b654677281fdb551cd77e0de75e3a5fa67a0987`; pinned apio
`1023d866849694417ced497e17e98a2cd2bd026e` (`include/apio.h`, `include/apio_reg.h`);
[apio upstream scope](https://github.com/piersfinlayson/apio),
[epio scope and limitations](https://github.com/piersfinlayson/epio), and
[Raspberry Pi chip documentation](https://www.raspberrypi.com/documentation/microcontrollers/microcontroller-chips.html).
The waveform/duration/count choices in this document are proposed experiment
settings; authoritative Zorro limits are a required E6 input, not asserted here.
