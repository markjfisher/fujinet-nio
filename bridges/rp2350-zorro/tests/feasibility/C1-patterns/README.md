# C1-patterns: known pattern capture

**C1 is a two-board W0 experiment.** The RP2040 drives 28 asserted `/AS`
transactions at 100 kHz PIO rate. The ordered values are `0..15`, eight
alternating `A,5` values, then the fixed seeded tail `6,D,3,C`. The Core2350B
must report exactly those 28 captured words, 28 capture IRQs, and no omitted or
extra word.

Each `/AS` low pulse and its preceding setup interval is 100 us. The released
hold interval is manifest-declared per transition: 110 us in the linear/tail
groups, 120 us between alternating values, and 130 us at group boundaries. The
extra PIO control instructions cause those holds; they are measured evidence,
not an analyser tolerance.

This uses the C0 capture APIO implementation and the shared RAM-only observer
firmware. Its C1 stimulus is APIO source in `src/`; host EPIO tests verify the
instruction words, output/strobe sequence, completion, rearm and abort. No PIO
text file is used.

Wiring remains W0: RP2040 GP2–GP5 to Core2350B GP2–GP5, RP2040 GP6 (`/AS`) to
Core2350B GP1, shared ground, analyzer CH1–CH4 on D0–D3, and CH8 on `/AS`.
Keep signals at 3.3 V. The analyzer verifies 28 low pulses and their sampled
values; the Core's USB report independently verifies captures and IRQ counts.

From this directory:

```sh
./run.sh build
./run.sh doctor
./run.sh load --usb-path 7-1.3.3.4.4 --dut-usb-path 7-1.3.3.4.3
./run.sh run --output /tmp/c1-run-001
python3 ../report_summary.py /tmp/c1-run-001/report.json
```

The first C1 `load` replaces the RP2040 Debug Probe firmware with this persistent
flash fixture, so put that RP2040 in BOOTSEL once. Give the physical path shown
by `doctor`; for this bench it is `7-1.3.3.4.4`:

```sh
./run.sh load --usb-path 7-1.3.3.4.4 --dut-usb-path 7-1.3.3.4.3
```

Later C1 loads use the same command with both boards connected normally:
picotool force-loads the RP2040 and Core2350B without BOOTSEL. `run` uses the
bound sessions. A pass requires both waveform and DUT evidence; offline
`analyse` remains waveform-only and incomplete.

Each physical C1 run also writes `waveform.svg`. It first identifies the burst
within the complete analyser acquisition, then renders the original D0–D3 and
`/AS` samples around it. Green lines mark every `/AS` falling edge, and the value
cells beneath them make the captured sequence readable against the raw waveform.
The SVG is visual evidence only; `report.json` remains the authoritative verdict.

The finite control protocol is lab equipment only. It has no FujiBus or future
bridge ABI meaning. See the [experiment index](../README.md) and the
[Story 2.2 plan](../../../docs/story-2-2-experiment-plan.md).
