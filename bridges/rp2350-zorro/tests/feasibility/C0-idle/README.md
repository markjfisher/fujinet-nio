# C0-idle: idle stimulus and capture suppression

**RP2040 stimulus implemented; DUT acceptance incomplete.** This runner builds,
RAM-loads and checks the C0 stimulus using the shared generator bench. A successful
run reports `stimulus_passed`, `experiment_status: incomplete`, and
`dut_evidence.status: not_observed`. It cannot establish that the RP2350 produced
no captures or IRQ activity: USB-observable DUT capture firmware is still needed.
The generator's internal completion IRQ is not DUT IRQ evidence.

The stimulus drives GPIO2–GPIO5 through 0–15 while GPIO6 (`/AS`) remains high.
It uses the shared idle-at-start console, finite-run control and release behavior.
The APIO program runs at 100 kHz; each data value spans 21 PIO clocks (210 µs).
Native epio tests check every cycle, completion, rearm and abort behavior against
an independent oracle. There are no `.pio` files or copied firmware projects.

Use the [Linux bench setup](../../../docs/bench-setup.md) and
[generator wiring](../generator-check/README.md). Connect analyzer D0–D3 to
GPIO2–GPIO5, D7 to GPIO6, and common ground. Keep the fixed 1 MHz acquisition rate.
The existing enrolled RP2040 profile is shared; C0 firmware and load sessions are
separate from generator-check. Switching experiments requires the corresponding
BOOTSEL RAM load. No flash is written.

From this directory:

```sh
./run.sh --help
./run.sh --dry-run
./run.sh build
./run.sh doctor
./run.sh configure                  # only if the bench is not enrolled
./run.sh load                       # selected RP2040 in BOOTSEL
./run.sh run --output /tmp/c0-run-001
./run.sh analyse --capture /tmp/c0-run-001/capture.sr
```

With no stage, `./run.sh` runs build, doctor, enrollment if needed, RAM load,
interactive wiring confirmation, acquisition, one finite run and analysis.
`--bench`, `--usb-path` and `--analyzer` have the same meanings as generator-check.
Help, dry-run, build and offline analysis need no connected hardware. A physical
run is always interactive; the analyzer must be acquiring before outputs run.

Analysis requires `/AS` high throughout the capture and exactly one complete
0–15 data sequence. Interior data holds must be 210 µs ±2 µs; first and last
holds must be at least 208 µs. Acquisition lead-in and released data pins make
the first/last exact hold boundaries unobservable. Data outside the validated
sequence may float; this check makes no assertion about those data values.
Metadata, sample rate, channel labels, truncation and unexpected strobe assertion
are checked. Exit zero means the requested stimulus stage succeeded, **not a
complete C0 DUT pass**. Reports retain this distinction for both run and analysis.

Build artifacts are in `build/stimulus-c0-rp2040/`; the default session is
`build/feasibility/C0/session.json`. Source/build identity includes this case's
APIO source and shared support. Result directories retain the manifest, board
session, console/acquisition logs, capture and analysis report.

See the [experiment index](../README.md) and
[Story 2.2 plan](../../../docs/story-2-2-experiment-plan.md). W0 remains a
prerequisite for the eventual combined stimulus/DUT acceptance.
