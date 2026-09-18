# Story 2.2 experiments

Start with **[generator-check](generator-check/README.md)**. It is the independent
RP2040 equipment check we already captured, now packaged for you to build, load,
run and analyse. It is **not C1 completion**: C1 needs asserted-strobe patterns
and ordered RP2350 captures. C0 now supplies the shared USB-observable capture
observer and proves the idle suppression baseline.

| Folder | Purpose | Status |
| --- | --- | --- |
| [generator-check](generator-check/README.md) | Independent 0–15 W0 waveform | Implemented; interactive runner plus separate stages |
| [C0-idle](C0-idle/README.md) | No capture without assertion; stimulus and DUT counters | Implemented; requires physical two-board run |
| [C1-patterns](C1-patterns/README.md) | Exact captured patterns and counts | Implemented; requires physical two-board run |
| [C2-held-active](C2-held-active/README.md) | One capture while /AS stays asserted | Planned |
| [C3-sampling-window](C3-sampling-window/README.md) | Data sampling transition | Planned |
| [C4-repetition](C4-repetition/README.md) | Pulse/gap limits | Planned |
| [C5-width-control](C5-width-control/README.md) | Wider data, direction and selection | Planned |
| [C6-pressure](C6-pressure/README.md) | FIFO pressure and explicit loss | Planned |
| [C7-reads](C7-reads/README.md) | Read response timing | Planned |
| [C8-turnaround](C8-turnaround/README.md) | Direction changes/output release | Planned |
| [C9-recovery](C9-recovery/README.md) | Reset, abort and recovery | Planned |
| [C10-real-bus](C10-real-bus/README.md) | Buffered actual-host validation | Planned; later hardware required |

C0 is retained: idle capture suppression is a useful baseline. Planned starters
fail explicitly without touching hardware. As each case is implemented, its
folder must contain its own source/configuration and expectations, point to
shared support code, and expose the same stage controls. This avoids separate
ad hoc projects and duplicated firmware support.

## Your controls

Run `generator-check/run.sh --help` from any working directory. The default
interactive flow and `all` build/check software, guide BOOTSEL identification and
local board enrollment when needed, load the manifest-selected fixture, and wait
for your explicit instruction before generating signals. Most current experiments
use a RAM-loaded RP2040; C1 deliberately installs its reusable generator fixture
in flash after one BOOTSEL install.
Separate `doctor`, `build`, `configure`, `load`, `run` and `analyse` stages let you inspect or
repeat individual steps. `--dry-run` previews operations without touching devices.
Builds never generate signals. No experiment stage runs sudo or changes system
permissions. Ctrl-C cancels the host workflow; the runner attempts stop and cleans
up acquisition, while firmware independently limits each burst.

Results belong in fresh directories under the bridge's ignored `build/` tree
(or the explicit `--output` path): source/artifact identity, console and tool logs,
raw `.sr` capture, expected/observed measurements and a machine-readable verdict.
Failed runs remain evidence; a busy analyzer or missing capture cannot pass.
PulseView can open saved captures once sigrok-cli releases the device.
Build and loader command logs are retained under `build/feasibility/stage-logs/`.

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

To implement C1 later (it is not implemented by this refactor):

1. Supply `C1-*/src/stimulus_program.c` and `src/stimulus_expectations.c`, exporting
   `stimulus_expected` using the contract above. Keep the expectations independent
   of the program being checked.
2. Fill in that directory's `experiment.json`, `README.md` and thin `run.sh`
   wrapper. Set an explicit bridge-relative `source_dir`, firmware preset/target,
   analyzer mapping and behavioral analysis fields. Reuse an existing
   `analysis_kind` when its checks describe the new waveform.
3. Add one `add_stimulus_test(directory executable ctest_name)` registration in
   [stimulus_tests.cmake](../../cmake/stimulus_tests.cmake).
4. Add configure/build presets in [CMakePresets.json](../../CMakePresets.json),
   with a separate binary directory, `STIMULUS_EXPERIMENT` directory name and
   `STIMULUS_TARGET`. No generic CMake branch or shared test edit is needed.
5. Run the experiment's `run.sh build`: it tests both native configurations and
   builds the RAM firmware. Then use its existing load/run/analyse stages for
   physical validation.

This contract covers the current W0 fixture: four data bits, a periodic optional
active-low strobe, seven program words and the shared 16-value console protocol.
A later experiment needing a new behavior (such as variable-length phases or
different pin groups) should extend that capability explicitly, not add an
experiment-name conditional. Provenance hashes the manifest-selected source
directory (including its oracle), shared harness/contract and CMake inputs.

The [Story 2.2 plan](../../docs/story-2-2-experiment-plan.md) remains authoritative
for test-first PIO behavior, fixture changes, evidence and later real-bus gates.
