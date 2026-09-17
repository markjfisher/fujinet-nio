# Generator check: repeatable W0 stimulus validation

This validates the **RP2040 generator and analyzer path**. It does not validate
RP2350 capture and does not complete C1. The RP2350 firmware is left alone.

## Start here

For initial setup, run the bridge's [setup script](../../../scripts/setup.sh).
If a build reports dirty vendored sources, run it with `--repair`; it preserves
the altered trees before restoring the pins. The [setup guide](../../../README.md#reproducible-setup)
also covers optional compiler installation. You do not need to run bootstrap
or individual CMake commands yourself.

From the workspace:

```sh
repos/fujinet-nio/bridges/rp2350-zorro/tests/feasibility/generator-check/run.sh
```

The starter explains its stages, builds the same pinned project and native tests,
then tells you when to use BOOTSEL. It waits for the intended RP2040, verifies its
identity, loads a RAM image and follows its physical USB port to the new console.
It asks before generating a burst. Normal-user access must be configured first;
see [one-time access setup](../README.md#one-time-linux-access-setup).

Run from this folder for individual steps:

```sh
./run.sh --help
./run.sh doctor
./run.sh --dry-run
./run.sh build
./run.sh load
./run.sh run
./run.sh analyse --capture ../../../docs/feasibility/results/2026-09-17-generator/w0-final-001.sr
./run.sh analyse --capture ../../../docs/feasibility/results/2026-09-17-generator/w0-final-001.sr --output /tmp/w0-reanalysis-001
```

`build` does not load or drive hardware. `load` loads idle firmware into RAM, not
flash; power cycling restores the existing flash firmware. `run` requires a
matching load-session identity and explicit start; rerun it for another fresh
capture without rebuilding. `analyse` rechecks a saved capture without a board,
console or analyzer. Use `--output` to name a new results directory; existing
physical results must not be silently overwritten. `--dry-run` has no build or
hardware side effects. Stop with Ctrl-C at any wait or experiment stage.

Build records the source/configuration identity beside the ELF. Load checks that
record and uses an immutable copy of the validated image, so the evidence names
the bytes actually loaded. Rebuild through this starter if that record is missing
or no longer matches the image. Offline analysis prints its verdict; `--output`
also retains a report in a new directory.

The configured generator flash ID is `754765170F445253`. The RP2350 DUT is
`DCD9EB3F6D168102` and must not be loaded by this starter. USB bus addresses change;
the starter discovers them. RAM serial `EEEEEEEEEEEEEEEE` is not a unique board
identity. If you change boards, inspect/update the manifest and identify the new
board deliberately; do not substitute an arbitrary ttyACM port.

## Wiring and expected result

| Signal | RP2040 GPIO | Existing Core2350B connection | Analyzer label / sigrok channel |
| --- | --- | --- | --- |
| D0 (LSB) | GP2 | GP2 input | CH1 / D0 |
| D1 | GP3 | GP3 input | CH2 / D1 |
| D2 | GP4 | GP4 input | CH3 / D2 |
| D3 | GP5 | GP5 input | CH4 / D3 |
| /AS | GP6 | GP1 input | CH8 / D7 |
| GND | GND | GND | Ground |

GPIO numbers are not header positions. Use common GND and 3.3 V signals; do not
join independently powered supply rails. CLK and CH5–CH7 are unused. The generator
releases pins at idle and weakly pulls /AS high. Signal ownership is generator
output to DUT input; no DUT response is needed for this equipment check.

Close PulseView's live analyzer connection before an automated run. The runner
arms sigrok acquisition before issuing the USB `run` command. Expect exactly
16 assertions with values 0 through 15. At the nominal 100 kHz PIO clock, each
low pulse is 100 us, consecutive assertions are 300 us apart, and measurable
setup/hold intervals are 100 us. Capture at 1 MHz; 1 us sample resolution is not
calibrated absolute clock accuracy. First zero-valued data setup can be invisible
against idle zero, and final data release is not a valid hold-time measurement.

A PASS requires both a fresh firmware completion and a valid independently
checked waveform. A failed acquisition, incorrect/missing edges, wrong channel
mapping, or wrong sample rate cannot count as a pass. Results retain `.sr`,
console/tool logs, firmware identity and a report. Open the saved `.sr` in
PulseView after the runner exits to inspect the signal yourself.

## Inspect the implementation

- [experiment.json](experiment.json): identity, pins, analyzer mapping and literal expectations.
- [src/stimulus_program.c](src/stimulus_program.c): this experiment's APIO waveform and register configuration, compiled unchanged into hardware and native tests.
- [shared RP2040 entry point](../../../lab/rp2040/main.c): SDK loading, GPIO ownership and USB handling.
- [shared control](../../../lab/rp2040/stimulus_control.c) and [header](../../../lab/rp2040/stimulus.h): bounded commands/session behavior.
- [native epio tests](../test_stimulus.c): independent waveform/configuration and control tests.
- [experiment runner](../experiment.py) and [runner tests](../test_experiment.py): staging, identity, acquisition and analysis.
- [firmware target](../../../cmake/stimulus.cmake) and [native target](../../../cmake/host.cmake): same source selection within Story 2.1's skeleton.

The [earlier evidence](../../../docs/feasibility/results/2026-09-17-generator/report.md)
contains four complete captured bursts and stop/reconnect checks. Those records
validate that firmware at that time; they are not evidence that this new starter
has completed a fresh physical run. Later C0/C1 experiments add the DUT observer.
