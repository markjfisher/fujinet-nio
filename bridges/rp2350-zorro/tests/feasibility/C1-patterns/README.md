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

Run `doctor` once when setting up or diagnosing the bench, then save the stable
physical paths in the ignored local bench profile:

```sh
./run.sh doctor
./run.sh configure-paths --usb-path GENERATOR_PORT --dut-usb-path DUT_PORT
```

Subsequent physical runs are one command:

```sh
./run.sh all --output /tmp/c1-run-001
```

`all` builds, force-loads both boards using the saved paths, waits for Enter,
captures and analyses the waveform, collects DUT counters, and prints the saved
report summary. Repeat `configure-paths` after moving a cable; explicit paths
remain available as a one-off override. The flash-load path force-loads the
connected RP2040 and Core2350B, so normal C1 runs do not require BOOTSEL. A pass
requires both waveform and DUT evidence; offline `analyse` remains waveform-only
and incomplete.

Each physical C1 run also writes `waveform.svg`. It renders the original D0–D3
and `/AS` samples around the analysed burst. Boundary markers use a solid
dark-green `A` for accepted, dashed orange `R` for rejected and dotted yellow
`?` for uncertain observations; value cells map the sequence to the raw waveform.
The SVG is visual evidence only; `report.json` remains the authoritative verdict.

The finite control protocol is lab equipment only. It has no FujiBus or future
bridge ABI meaning. See the [experiment index](../README.md) and the
[Story 2.2 plan](../../../docs/story-2-2-experiment-plan.md).
