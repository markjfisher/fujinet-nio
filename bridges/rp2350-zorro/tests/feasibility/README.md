# Story 2.2 experiments

Start with **[generator-check](generator-check/README.md)**. It is the independent
RP2040 equipment check we already captured, now packaged for you to build, load,
run and analyse. It is **not C1 completion**: C1 needs asserted-strobe patterns
and ordered RP2350 captures. C0 now supplies the shared USB-observable capture
observer and proves the idle suppression baseline.

| Folder | Purpose | Status |
| --- | --- | --- |
| [generator-check](generator-check/README.md) | Independent 0–15 W0 waveform | Implemented; interactive runner plus separate stages |
| [C0-idle](C0-idle/README.md) | No capture without assertion; stimulus and DUT counters | Passed current W0 bench; no `/AS` capture/IRQ while data changes |
| [C1-patterns](C1-patterns/README.md) | Exact captured patterns and counts | Passed current W0 bench; 28 ordered captures |
| [C2-held-active](C2-held-active/README.md) | One capture while /AS stays asserted | Passed current W0 bench; later held-active values do not recapture |
| [C3-sampling-window](C3-sampling-window/README.md) | Data sampling transition | Passed current W0 bench at the declared 10/50 µs offsets |
| [C4-repetition](C4-repetition/README.md) | Repeated low-width/gap sweep | Passed current W0 bench; failure-boundary sweep remains open |
| [C5-width-control](C5-width-control/README.md) | W1 selected-write width and control | Passed current W1 bench; analyzer subset plus full DUT words |
| [C6-pressure](C6-pressure/README.md) | Bounded FIFO stall, explicit loss and recovery | Implemented; passed on the current W1 bench |
| [C7-reads](C7-reads/README.md) | Read response timing | Passed current W1 functional bench; electrical/timing margin remains open |
| [C8-turnaround](C8-turnaround/README.md) | Direction changes/output release | Passed current W1 functional bench; external release/no-contention measurement remains open |
| [C9-recovery](C9-recovery/README.md) | Reset, abort and recovery | Passed fresh-rearm case; power-cycle/disconnect injection remains open |
| [C10-real-bus](C10-real-bus/README.md) | Passive actual-host evidence capture | Implemented collector; reviewed real-bus mapping/buffering and hardware evidence pending |

C0 is retained: idle capture suppression is a useful baseline. Planned starters
fail explicitly without touching hardware. As each case is implemented, its
folder must contain its own source/configuration and expectations, point to
shared support code, and expose the same stage controls. This avoids separate
ad hoc projects and duplicated firmware support.

## Your controls

Run `generator-check/run.sh --help` from any working directory. `doctor` is for
initial setup and diagnosis. Once an RP2040 is enrolled, save its stable physical
generator/DUT USB topology paths with `configure-paths`; the ignored local bench
profile retains those selections. Then an implemented two-board experiment runs
with `./run.sh all --output NEW_DIRECTORY`: it builds, loads both selected images,
waits for your explicit Enter before generating signals, acquires, analyses,
collects DUT evidence, and prints the saved report summary. C0 still needs
RP2040 BOOTSEL on each RAM load; C1–C9 force-load their connected flash fixtures
without BOOTSEL.

Separate `doctor`, `build`, `configure`, `configure-paths`, `load`, `run` and
`analyse` stages remain available for inspection and recovery. `--dry-run`
previews operations without touching devices. Builds never generate signals. No
experiment stage runs sudo or changes system permissions. Ctrl-C cancels the host
workflow; the runner attempts stop and cleans up acquisition, while firmware
independently limits each burst.

Each manifest declares its normal bounded analyser duration: C0–C6 use 50 ms at
1 MHz. Set
`"acquisition_seconds"` when a later case needs a different normal window, or
override one run with, for example, `./run.sh run --acquisition-seconds 1`. The
selected duration and sample count are retained in `report.json`.

Results belong in fresh directories under the bridge's ignored `build/` tree
(or the explicit `--output` path): source/artifact identity, console and tool logs,
raw `.sr` capture, expected/observed measurements and a machine-readable verdict.
Failed runs remain evidence; a busy analyzer or missing capture cannot pass.
PulseView can open saved captures once sigrok-cli releases the device.
Physical runs also generate `waveform.svg`: its manifest-owned purpose statement,
raw analyser lanes, classified transaction boundaries, decoded values and DUT
evidence make the saved capture inspectable. It is a debug aid alongside the raw
`capture.sr`; `report.json` remains the authoritative verdict and neither file
claims an unmeasured internal PIO sample-clock position.

Every implemented manifest carries a `waveform_view` description. Its ordered
`lanes` contain ordinary signals (`label`, `channel`, `role`, `polarity`) and
data buses (`bits`, each with its displayed label, analyzer `channel` and bus
bit number). `transactions` supplies human-facing boundary, expected-value,
classification and optional DUT-metric labels. The analyser copies that description into
`report.json` as `waveform.visualization`; the SVG renderer reads the report,
not an experiment directory name. Channels use sigrok names such as `D0` and
are unbounded: a later analyzer can declare `D47`, for example, when its raw
capture uses a wider `unitsize`. Transaction records can declare
`classification` as `accepted`, `rejected` or `uncertain`; their boundaries and
sampled values are rendered directly. Only `idle` and semantic layouts such as
sampling-window/repetition retain small `analysis_kind` presentation hooks.
The visual mapping presents Sigrok `D0` as physical `CH1` (and so on); use an
optional `physical_channel` lane field when another analyzer needs a different
front-panel name.
An intentional bounded-loss case can set `dut.allows_partial_capture`; it must
still declare the exact DUT values as an ordered subsequence of generator
assertions. C6 uses this only to expose `PUSH BLOCK` loss, never to waive a
counter mismatch.
The analyser also derives `waveform.analyzer_coverage`: each data bus records
its width and observed bit numbers, controls record their labels/channels, and
`summary` makes partial instrumentation explicit, for example `3/16 data bits
+ 5 controls observed`. The terminal summary and SVG print that same statement.
Build and loader command logs are retained under `build/feasibility/stage-logs/`.
The [synthetic-bench evidence ledger](../../docs/feasibility/bench-evidence.md)
records the reviewed local C0–C9 run IDs and the limits of those claims.

Summarise an existing report without rerunning its analysis or changing its verdict:

```sh
python3 tests/feasibility/report_summary.py build/feasibility/C0/<run>/report.json
```

The summary shows recorded status/evidence/provenance and uses the report's
`analysis_kind` only to select a presentation formatter. It does not validate a
capture or make an independent pass/fail decision.

For another user or computer, follow [bench setup and portability](../../docs/bench-setup.md).
Enrollment saves a per-bench flash identity; USB permission rules match shared
device types and contain no private board serial.

## One-time Linux access setup

First run `generator-check/run.sh doctor`. Build prerequisites are the bridge's
[existing toolchain setup](../../README.md); capture additionally uses
`sigrok-cli`, the installed fx2lafw firmware and a USB-enabled pinned picotool.
Close PulseView's live device while sigrok-cli owns the analyzer.

Use `../../scripts/setup.sh` for software setup, or add `--repair` to preserve and
restore accidentally changed dependency sources. These scripts never alter USB
permissions; the optional access setup below is separate.

If normal-user access to RP2040 BOOTSEL or its serial console is missing, review
[69-nio-feasibility.rules](69-nio-feasibility.rules). For a systemd desktop session,
you may install it **once**, explicitly, from this directory:

```sh
sudo install -m 0644 69-nio-feasibility.rules /etc/udev/rules.d/69-nio-feasibility.rules
sudo udevadm control --reload-rules
```

Reconnect the boards after installation. The rule grants the active local desktop
user access to RP2040 BOOTSEL (`2e8a:0003`) and SDK USB/serial (`2e8a:000a`), plus
the Core2350B/RP2350 USB and CDC console (`2e8a:0009`) used by the C0 DUT. It
covers those device classes, not just this board; it does not select a target or
authorize a load. The runner separately verifies the intended flash identity and
physical USB path. Your analyzer's packaged sigrok rules remain responsible for
analyzer access. The `69-` ordering is intentional: the tag must exist before
systemd's `73-seat-late.rules` applies it.

For SSH/headless hosts without an active local seat, this rule may not grant
access: arrange an appropriate device-access group with the machine administrator
before running hardware stages. The runner reports missing access and stops;
it does not fall back to arbitrary devices or a sudo retry. No device-specific
`setfacl` sequence from the original session is part of the repeatable procedure.

## Extending the stimulus tests

The shared [host test](test_stimulus.c) links the selected experiment's APIO
program and its independent `src/stimulus_expectations.c` oracle. The small
[contract](stimulus_expectations.h) describes instruction words/count, data
values, first-data cycle and cadence, the low strobe interval within each value,
completion IRQ cycle, observation length and an abort point. Cycles are zero-based
observations after each epio step. A zero low duration means /AS always stays high.
The test checks every cycle, completion, repeat runs and rearm after abort.
Expectation data is host-only and is not compiled into the RP2040 firmware.

`generator-check` supplies 30-cycle values with /AS low during cycles 10–19
of each value, and completion at cycle 481. `C0-idle` supplies 21-cycle values,
no low strobe interval and completion at cycle 347. Both observe 550 cycles;
their program words and timing expectations live beside their respective programs.
These are generator tests: they provide no DUT capture/IRQ evidence.

To add a later experiment:

1. Supply `Cx-*/src/stimulus_program.c` and `src/stimulus_expectations.c`, exporting
   `stimulus_expected` using the contract above. Keep the expectations independent
   of the program being checked.
2. Fill in that directory's `experiment.json`, `README.md` and thin `run.sh`
   wrapper. Set an explicit bridge-relative `source_dir`, firmware preset/target,
   analyzer mapping and behavioral analysis fields. Reuse an existing
   `analysis_kind` when its checks describe the new waveform.
3. Add its `waveform_view` lanes and transaction labels. Declare every captured
   bit/control signal explicitly; do not make the renderer learn the new
   experiment ID. A subset is valid when the manifest identifies its observed
   data bits and the analysis report records the corresponding limitation.
4. Add one `add_stimulus_test(directory executable ctest_name)` registration in
   [stimulus_tests.cmake](../../cmake/stimulus_tests.cmake).
5. Add configure/build presets in [CMakePresets.json](../../CMakePresets.json),
   with a separate binary directory, `STIMULUS_EXPERIMENT` directory name and
   `STIMULUS_TARGET`. No generic CMake branch or shared test edit is needed.
6. Run the experiment's `run.sh build`: it tests both native configurations and
   builds the selected firmware. Then use its existing load/run/analyse stages for
   physical validation.

The contract covers W0 phase-based stimuli and C5's wider DMA-fed output stream.
C5 adds `output_words` to the host-only oracle; the shared host test models FIFO
service and validates complete bus-state changes without CPU-timed edges. A later
experiment needing a new behavior should extend that capability explicitly, not
add an experiment-name conditional. Provenance hashes the manifest-selected source
directory (including its oracle), shared harness/contract and CMake inputs.

The [Story 2.2 plan](../../docs/story-2-2-experiment-plan.md) remains authoritative
for test-first PIO behavior, fixture changes, evidence and later real-bus gates.
