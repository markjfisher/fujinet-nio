# C0-idle: idle stimulus and capture suppression

**C0 is a two-board experiment.** It builds and RAM-loads the RP2040 stimulus and
Core2350B DUT images. Before the finite stimulus burst, the runner resets the
DUT's counters. Afterwards it collects the DUT's USB `report` response. C0 passes
only if waveform analysis succeeds and the DUT reports `capture_count=0` and
`capture_irq_count=0`. The generator's completion IRQ is not DUT IRQ evidence.

The stimulus drives GPIO2–GPIO5 through 0–15 while GPIO6 (`/AS`) remains high.
It uses the shared idle-at-start console, finite-run control and release behavior.
The APIO program runs at 100 kHz; each data value spans 21 PIO clocks (210 µs).
Native epio tests check every cycle, completion, rearm and abort behavior against
an independent oracle. There are no `.pio` files or copied firmware projects.

Use the [Linux bench setup](../../../docs/bench-setup.md) and
[generator wiring](../generator-check/README.md). Connect RP2040 GP2–GP5 to DUT
GP2–GP5, and RP2040 GP6 (`/AS`) to DUT GP1. Connect analyzer CH1–CH4 to the four
data nets and CH8 to `/AS`; connect analyzer GND and both board grounds together.
Keep all fixture signals at 3.3 V and the fixed 1 MHz acquisition rate.
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
./run.sh load --dut-usb-path 1-2.3
./run.sh run --output /tmp/c0-run-001
./run.sh analyse --capture /tmp/c0-run-001/capture.sr
python3 ../report_summary.py /tmp/c0-run-001/report.json
```

With no stage, `./run.sh --dut-usb-path ...` runs build, doctor,
enrollment if needed, RAM-loads the RP2040 and then the Core2350B, requests an
interactive wiring confirmation, acquires one finite run and collects both reports.
For each `load`, first put the requested board alone into BOOTSEL when prompted.
`--bench`, `--usb-path` and `--analyzer` have the same meanings as generator-check.
Help, dry-run, build and offline analysis need no connected hardware. A physical
run is always interactive; the analyzer must be acquiring before outputs run.

Analysis requires `/AS` high throughout the capture and exactly one complete
0–15 data sequence. Interior data holds must be 210 µs ±2 µs; first and last
holds must be at least 208 µs. Acquisition lead-in and released data pins make
the first/last exact hold boundaries unobservable. Data outside the validated
sequence may float; this check makes no assertion about those data values.
Metadata, sample rate, channel labels, truncation and unexpected strobe assertion
are checked. A physical run exits zero only after the DUT counter report matches
the manifest. Offline `analyse` validates the waveform only and remains
`stimulus_passed`/`incomplete`, because it has no DUT evidence.

Build artifacts are in `build/stimulus-c0-rp2040/` and `build/dut-c0-rp2350/`;
the generator and DUT sessions are `build/feasibility/C0/session.json` and
`dut-session.json`. Result directories retain both console logs, capture and the
combined analysis report.

See the [experiment index](../README.md) and
[Story 2.2 plan](../../../docs/story-2-2-experiment-plan.md). W0 remains a
prerequisite for the eventual combined stimulus/DUT acceptance.
