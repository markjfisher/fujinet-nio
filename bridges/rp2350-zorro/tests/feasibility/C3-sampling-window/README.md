# C3-sampling-window: safe data-transition offsets

**C3 is a two-board W0 experiment.** It maps four declared data-change offsets
around `/AS` assertion at the existing 100 kHz RP2040 PIO rate. It does not claim
to expose the exact internal RP2350 PIO `IN PINS` clock.

| Case | Data transition | Expected DUT capture | `/AS` low |
| --- | --- | --- | --- |
| pre-10 | `0 -> 1`, 10 us before assertion | `1` (new) | 20 us |
| post-10 | `2 -> 3`, 10 us after assertion | `2` (old) | 20 us |
| pre-50 | `4 -> 5`, 50 us before assertion | `5` (new) | 10 us |
| post-50 | `6 -> 7`, 50 us after assertion | `6` (old) | 60 us |

The RP2040 PIO program owns these timing relationships. The analyser verifies
each external data-to-`/AS` offset to within its 1 MHz sample resolution and the
Core2350B must report exactly four captures, four capture IRQs and values
`1,2,5,6`. A value at the opposite side of one of these declared safe offsets is
a failure. The unresolved region between the 10 us before and 10 us after cases
remains explicitly uncharacterized; it is not a Zorro timing limit.

Wiring is unchanged from W0: RP2040 GP2–GP5 to Core2350B GP2–GP5, RP2040 GP6
(`/AS`) to Core2350B GP1, shared ground, analyzer CH1–CH4 on D0–D3 and CH8 on
`/AS`. Keep every signal at 3.3 V. The waveform runs once only after the runner
has armed the analyzer and you press Enter.

```sh
./run.sh build
./run.sh doctor
./run.sh load --usb-path 7-1.3.3.4.4 --dut-usb-path 7-1.3.3.4.3
./run.sh run --output /tmp/c3-run-001
python3 ../report_summary.py /tmp/c3-run-001/report.json
```

C3 installs the reusable RP2040 fixture in flash like C1 and C2. With the
existing flash fixture, both boards stay connected normally and `picotool`
force-loads them; a fresh RP2040 that still runs the Debug Probe needs one
initial BOOTSEL install. `./run.sh --dry-run` prints the selected images, the
0.25-second / 250,000-sample default acquisition and every other planned step.
Use `--acquisition-seconds N` only when a longer diagnostic capture is useful.

Each physical run preserves `capture.sr`, USB logs, `report.json` and
`waveform.svg`. The SVG shows the full acquisition and a raw D0–D3/`/AS` zoom;
green lines mark the four capture transaction boundaries. The value cells and
summary map each captured value to its before/after case. These are external
observations. They do not turn the analyser edge into an internal PIO clock
marker.

C3 uses APIO stimulus source, a separate EPIO stimulus oracle and an EPIO DUT
test. The shared feasibility DUT observer and report protocol are unchanged; no
ESP32 code, bridge ABI or production timing claim is introduced.
