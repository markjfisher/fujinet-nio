# Story 2.2 — RP2350B Zorro-facing feasibility experiment

Status: active feasibility plan, updated 2026-09-19. The reusable W0 framework,
generator, Core2350B observer and C0–C5 are implemented. Local physical reports
show passed two-board runs for C0–C4; their raw captures, console logs, firmware
hashes and SVG evidence are retained under `build/feasibility/`. Those results
establish the stated synthetic-fixture behavior only. They do not establish a
Zorro-II timing margin, output safety, sustained transfer capacity, or real-bus
compatibility. C5 awaits its first physical W1 run; C6–C10 remain planned work
in this same Story 2.2.

## Repeatable experiment contract — user amendment, 2026-09-17

Every implemented experiment must be independently runnable and inspectable from
its own folder under `tests/feasibility/`, using one `run.sh` starter. The
[experiment index](../tests/feasibility/README.md) distinguishes the implemented
`generator-check` equipment validation from the C0–C10 matrix. Prior generator
captures support E0–E2, but do not complete C1 or establish DUT behavior.

Each implemented folder contains its specific source/configuration, README,
expected observations and tests or links to focused shared tests. It references
shared board/USB/APIO support rather than copying another project. The starter
exposes build, load, run and analyse separately, and `all` is the visible,
interactive build/load/run/analyse/report flow. It loads only its verified
targets, waits for re-enumeration, explains signals and waits for the user to
start. C0's RAM generator load requires RP2040 BOOTSEL; C1–C5 use the enrolled
flash generator and force it into the loader without a button press. Preserve raw
capture/logs, firmware hashes, failures and expected/observed verdicts in fresh
output directories. Offline analysis and builds do not require connected boards.

Permission setup is an explicit one-time prerequisite, checked by a doctor stage;
no repeated ad hoc sudo commands, implicit system-policy changes or agent-private
temporary harnesses are an accepted experiment interface. Dry-run and Ctrl-C
provide preview and cancellation. Never claim pass from a stale console greeting,
failed analyzer acquisition or an unverified USB identity. The user permits
replacing the RP2040 debug firmware, but RAM-only loading remains sufficient for
the current generator check; no persistent flash change is required.

C0–C4 are real two-board experiments, not placeholders. C5 has buildable
two-board W1 firmware and awaits physical evidence; C6–C10 declare their missing
software/hardware and refuse execution until implemented. Completing W0 does not
close the full E0–E7 or real-bus gates.

## Goal and relationship to Story 2.1

Determine which timing-sensitive bus-facing operations an RP2350B can perform,
under what conditions, and with what measured margins. Start with independent
3.3 V synthetic stimulus, then validate representative real-bus conditions when
the passive Zorro-II breakout and A500/Zorro-II adapter are available.

Story 2.1 supplies [the isolated project](../README.md), pinned Pico SDK/apio/epio,
separate host/firmware builds, shared APIO capture code, and enforced PIO policy.
Story 2.2 now adds the lab-only `feasibility_dut` observer, independent RP2040
stimulus, a repeatable experiment runner, external traces and machine-readable
reports. The production `bridge_capture` firmware still discards words and has
USB stdio disabled; the observer is deliberately not a production bridge ABI.

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
| Logic analyzer | Available: inexpensive eight-channel USB fx2lafw-compatible unit labelled CH1–CH8 plus GND/CLK; used through sigrok-cli/PulseView at 1 MHz for W0. Its electrical limits and higher simultaneous sample-rate capability remain to be recorded before tighter claims. |
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

W0 is the implemented, passing synthetic fixture for C0–C4. Wire with both USB
supplies disconnected, and use the multimeter to check GPIO-to-GPIO connections
and unintended shorts before powering. The analyzer, independent RP2040 and DUT
USB report provide complementary evidence: the waveform proves the observed nets;
the DUT report proves the PIO-to-ARM capture path. They are still not a completed
electrical continuity record for another bench or a Zorro bus validation.

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

The unit is labelled CH1–CH8 plus GND/CLK as reported by the user. CH5–CH7
are unused. Confirm the reported GND terminal is ground, then connect it to the
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

C5 uses W1 only as a selected-write input fixture. Its eight analyzer inputs are
`/AS`, `SELECT`, `R/W`, `/UDS`, `/LDS`, and representative D0, D8 and D15.
The exact CH1–CH8 mapping and its scope are recorded in
[`C5-width-control/README.md`](../tests/feasibility/C5-width-control/README.md).
It cannot be a simultaneous 16-bit analyzer trace; the complete ordered word
evidence comes from the DUT PIO RX FIFO drained by ARM.

## Firmware and test architecture

The generator executes finite, preloaded PIO waveform sequences independently of
the DUT clock. USB configures and retrieves results; it does not pace individual
edges. Monitor generator underrun, sequence completion and read-sampling capacity.
A starved generator or incomplete trace invalidates that run, rather than counting
as DUT success or failure. Use bounded buffers; prepare SRAM/DMA feeding only when
needed and validate its effect on the generated waveform.

The DUT compiles the shared `src/capture_program.c`. The separate feasibility
executable adds USB CDC reporting while preserving the production skeleton. It
collects data in bounded RAM with counters for received words and PIO IRQs; verbose
output is deferred until the timed burst ends.
Compare CPU polling/interrupt/DMA draining only as measured experiment variants,
not a permanent firmware architecture. Preserve input synchronization initially;
record any later change as a distinct experiment, never silently bypass it.

### PIO-to-ARM architecture coverage

The W0 experiments already use the intended capture ownership split on the
Core2350B. `PIO0 SM0` executes an APIO-defined loop: wait for `/AS` low, sample
the four input data pins, blocking-push the word to its RX FIFO, set a PIO IRQ,
then wait for `/AS` high before accepting another assertion. The feasibility ARM
firmware drains that RX FIFO into bounded RAM and reports the resulting counts and
ordered values over USB after the burst. C0–C4 therefore prove the narrow
hardware path **pin → PIO input shift register → PIO RX FIFO → ARM RAM**; the
USB console is reporting, never pacing individual bus edges. Their EPIO tests
exercise the same APIO capture source and assert FIFO and PIO-IRQ behavior.

This is deliberately a baseline, not yet the final transfer architecture. The
current ARM loop polls both the FIFO and the PIO IRQ; it has no NVIC PIO interrupt
handler, DMA channel, address/select state machine, output state machine, or
bounded descriptor/ownership hand-off. `PUSH BLOCK` stalls the capture state
machine when its RX FIFO fills. That is safe against silent overwrite, but it can
miss later assertions while stalled. C0–C4 are short enough that they do not
measure this limit.

The remaining experiments must preserve this division of responsibility rather
than replacing timed PIO work with CPU GPIO loops:

| Gate | Required architectural evidence |
| --- | --- |
| C5 | **Implemented; physical W1 evidence pending.** Core2350B PIO0 SM0 uses an APIO capture loop over GP2–22: begin from released `/AS`, wait for `/AS`, `IN PINS,21`, blocking-push and PIO IRQ. ARM drains bounded raw records and applies the selected-write qualifier, retaining counts for accepted and rejected observations. RP2040 PIO0 SM0 uses ten APIO instructions and one DMA channel to emit 45 complete GP2–22 states; the CPU does not pace waveform edges. |
| C6 | Measure the PIO RX FIFO → ARM transfer under deliberate pressure. Compare the current polling baseline with an NVIC-IRQ and/or DMA drain variant, record stall/loss boundaries and select the viable bounded-transfer design. |
| C7–C8 | Use a separately defined PIO output/turnaround path with data preloaded by ARM or DMA. ARM may arm and replenish bounded buffers; it must not toggle timing-critical response pins per access. |
| C9 | Prove that reset/abort releases each PIO-owned output and invalidates any ARM-side records from the old run. |

No production mailbox/register ABI is implied by these gates. They establish
whether PIO-front-end capture plus a bounded PIO-to-ARM transfer can support the
eventual bridge architecture.

**APIO/RP2040 compatibility gate:** pinned apio v0.3.0 is RP2350-oriented and its
`include/apio_reg.h` has RP2350 reset/pad/PIO register addresses. It must not perform
hardware initialization on RP2040. The lab implementation uses a shared C
stimulus builder with APIO instruction-encoding macros and an RP2040 Pico SDK
loader/configuration shim. The builder's emitted words and configuration manifest
are used by the native epio stimulus tests and by the RP2040 loader; no hand-copied
instruction arrays, `.pio` source or pioasm build step. Restrict instructions and
configurations to the RP2040-supported subset and verify them against the pinned
SDK/RP2040 documentation. E0 has verified this seam for the W0 instruction subset:
APIO supplies the encoded words while the RP2040 Pico SDK owns hardware setup.
That does not claim upstream APIO hardware-initialization support for RP2040.
Epio is an RP2350 emulator, so passing these shared-instruction tests does not
establish RP2040 hardware timing.

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
| C0 idle | /AS high; change data | **Implemented and passed on W0:** no captures or IRQ activity during the bounded observation |
| C1 patterns | All 16 four-bit values, alternating 0xA/0x5, then seeded sequences | **Implemented and passed on W0:** exact sequence/full words; one capture for each assertion, no silent discard |
| C2 held-active | Assert /AS once, hold it low while changing data | **Implemented and passed on W0:** one sample only; held duration does not create another capture |
| C3 sampling window | Change data at swept offsets before/after assertion | **Implemented and passed on W0:** before/after values at 10 and 50 us offsets; unresolved edge timing remains unmeasured |
| C4 repetition | Repeated assertions, decreasing high gap and low width separately | **Implemented and passed on W0:** 20 ordered captures across 100/50/20 us low and released-gap points. A failure-boundary sweep remains future work. |
| C5 width/control | 4 -> 8 -> 16 bits; walking bits, R/W, lane strobes, SELECT | **Implemented; physical W1 run pending:** 22 assertions comprising four ignored controls and 18 selected writes. The current PIO fixture uses 120 us `/AS` low and 110 us released intervals. Analyzer checks controls plus D0/D8/D15; DUT must report the 18 full ordered words. |
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
| [x] E0: portability and fixture definition | Shared `lab/rp2040/stimulus.h`, APIO instruction builders in each experiment source directory, Pico SDK loader/configuration shim and native EPIO stimulus tests | RP2040 builds and analyzer captures use the APIO words. W0 physical header mapping is recorded; 1 MHz analyzer evidence supports W0 functional/tens-of-microseconds observations, not tighter timing claims. |
| [x] E1: integrated targets | `CMakeLists.txt`, `CMakePresets.json`, `cmake/host.cmake`, `scripts/bootstrap.py`, policy and tooling tests | Host Debug/Release, RP2040 stimulus and RP2350B DUT presets build independently; unsupported board/platform configurations are rejected. |
| [x] E2: USB observability and runner | `src/feasibility_dut.c`, `lab/rp2040/main.c`, `tests/feasibility/experiment.py` and its host tests | Role-aware USB control, fresh-run evidence, bounded output release, DUT reset/report protocol and stored bench paths work through each experiment's `run.sh`. |
| [x] E3: physical four-bit reproduction | C0–C4 directories, shared `src/capture_program.c`, EPIO capture tests and generated `report.json`/`waveform.svg` evidence | Passed local W0 two-board C0–C4 reports establish exact functional behavior at the declared conditions. The failure boundary, high-rate endurance and external timing margin remain open. |
| [~] E4a: width/control | W1 C5 stimulus/capture sources, manifest and focused EPIO test | Implemented: PIO/DMA allocation and selected-write capture are covered by host tests; physical W1 evidence is pending. |
| [ ] E4b: pressure | Extend the W1 capture fixture for C6 | Measure FIFO-to-ARM pressure and explicit loss/stall outcomes; no false acknowledgements or unreported loss. |
| [ ] E5: reads/turnaround/recovery | Extend E4 APIO source/tests/cases, DUT logging and RP2040 receiver — C7–C9 | Both-side expectations, external read-valid/release measurements and local timeout/reset cleanup; bidirectional fixture reviewed before outputs enabled |
| [ ] E6: real-bus procedure and execution | `docs/feasibility/zorro-requirements.md`, `docs/feasibility/real-bus-procedure.md`, applicable `src/feasibility/` probe and focused Amiga exerciser only if needed — C10 | Breakout/adapter, reviewed buffering/power/direction and instruments available; cited bus limits and actual captures; no uncontrolled host address writes |
| [ ] E7: evidence and verdict | `docs/feasibility/report.md`, `docs/feasibility/results/`; workspace scope/acceptance link — audit case/requirement coverage | Software/bench/real-bus outcomes separated; reproducible evidence, measured margins and explicit proceed/hold with unresolved items |

E0 and E1 may have a minimal build dependency on each other; implement just enough
E1 plumbing to compile the E0 spike, then finish E1 before E2. E3 physical capture
can begin before E4/E5 or the real-bus hardware arrives. Missing equipment blocks
only the experiments needing it. Passing E3 alone cannot mark Story 2.2 physically
accepted. Implementation remains incremental within the existing skeleton.

The workspace exposes `scripts/build.sh rp2040-stimulus` as a discoverable,
build-only generator task; it neither loads a board nor drives pins. The
experiment-local `run.sh` remains the hardware interface because it owns the
selected manifest, enrolled identities, bench topology, interactive arming and
evidence directory. A future workspace wrapper may build all feasibility targets,
but must not silently flash boards, open consoles or drive pins.

## Implemented build/run interface

The existing regression command remains `scripts/build.sh rp2350`. For a W0
experiment, work in its own directory. After one-time enrollment and
`configure-paths`, the normal reproducible command is:

```sh
cd tests/feasibility/C5-width-control
./run.sh all --output /tmp/c5-run-001
```

`all` builds host Debug/Release tests and the selected RP2040/RP2350 firmware,
loads both images, resets the DUT counters, waits for explicit Enter before the
burst, acquires the analyzer trace, collects the DUT report, analyses both sources
of evidence, writes `report.json` and `waveform.svg`, then prints a summary.
`doctor`, `configure`, `configure-paths`, `build`, `load`, `run`, `analyse` and
`report` remain available for diagnosis and inspection; `all --dry-run` previews
the same sequence without touching hardware.

The RP2350B target remains `waveshare_core2350b` / `rp2350-arm-s`; the RP2040
stimulus target is `pico` / `rp2040`. Firmware and host builds use separate build
directories and share only the pinned dependency sources/picotool. C0 loads a
RAM-only generator and requires BOOTSEL. C1–C5 use a flash-installed generator,
so their normal `all` run force-reboots the known connected board into the loader
without a button press. The runner verifies enrolled identity and local topology
paths before it arms either board.

Run the owning experiment's `run.sh build` after a source/toolchain change, plus
`python3 scripts/check_pio_policy.py` and the focused runner tests when changing
the framework. Library or driver edits are not planned; a later Amiga exerciser
requires its own named native/targeted checks before implementation.

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

- TZT RP2040 clone exact revision, flash device/SDK configuration and complete
  physical header map. W0 has working mapped GPIOs; W1 remains provisional. The
  advertised memory size is user-reported, not a verified board specification.
- Eight-channel analyzer exact model, host capture software, input limits and
  simultaneous sample rate; HANMATEK DOS1102 probe configuration and usable
  measurement resolution. All three instruments are available, including the
  multimeter. Start with USB functional diagnostics and W0 traces; qualify each
  timing measurement against actual instrument settings.
- Breakout and A500 adapter revisions/schematics, buffering circuit and timing
  source editions; resolve before E6, not by importing a speculative production ABI.
- W0 verifies the APIO instruction-builder/RP2040-loader seam. It makes no claim
  of upstream APIO RP2040 hardware-initialization support; a wider W1 program
  needs its own instruction/configuration validation.

Sources inspected: Story 2.1 files at firmware revision
`6b654677281fdb551cd77e0de75e3a5fa67a0987`; pinned apio
`1023d866849694417ced497e17e98a2cd2bd026e` (`include/apio.h`, `include/apio_reg.h`);
[apio upstream scope](https://github.com/piersfinlayson/apio),
[epio scope and limitations](https://github.com/piersfinlayson/epio), and
[Raspberry Pi chip documentation](https://www.raspberrypi.com/documentation/microcontrollers/microcontroller-chips.html).
The waveform/duration/count choices in this document are proposed experiment
settings; authoritative Zorro limits are a required E6 input, not asserted here.
