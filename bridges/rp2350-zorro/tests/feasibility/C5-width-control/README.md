# C5-width-control: W1 selected-write capture

C5 is the first W1 experiment. The RP2040 PIO state machine drives all 21
contiguous W1 signals from DMA-fed words; it does not use CPU-timed GPIO. The
Core2350B PIO state machine accepts only `SELECT=1`, `R/W=0`, `/UDS=0`,
`/LDS=0` assertions, captures D[15:0] into its RX FIFO, and the ARM observer
reports the drained values after the burst.

The PIO program is deliberately scoped to this declared control sequence: its
four rejected assertions advance its qualifier waits, and the first all-qualifying
write is therefore the first possible capture. This proves C5's fixture rule; it
does not yet claim arbitrary control changes or FIFO-pressure behavior, which are
later cases.

The stimulus first issues four deliberately ignored assertions: unselected,
read, lower-lane-only and upper-lane-only. It then issues `0x000A`, `0x00A5`,
and sixteen 16-bit walking-one writes. The DUT must report exactly the resulting
18 values and IRQs. The C5 analyser checks every `/AS` assertion's observed
control state and D0/D8/D15; the full word sequence comes independently from
the DUT PIO-to-ARM report.

Connect the analyzer ground to the shared W1 ground. Leave its CLK pin open.

| Analyzer channel | W1 net | RP2040 / Core2350B GPIO |
| --- | --- | --- |
| CH1 / D0 | `/AS` | GP18 |
| CH2 / D1 | `SELECT` | GP22 |
| CH3 / D2 | `R/W` | GP19 |
| CH4 / D3 | `/UDS-like` | GP20 |
| CH5 / D4 | `/LDS-like` | GP21 |
| CH6 / D5 | D0 | GP2 |
| CH7 / D6 | D8 | GP10 |
| CH8 / D7 | D15 | GP17 |

This subset deliberately favors transaction qualification and representative
low/middle/high data bits. It is not a simultaneous 16-bit trace. The saved
`waveform.svg` labels ignored assertions in orange and selected writes in green;
the raw `capture.sr` remains available for PulseView.

After reconnecting USB, use `./run.sh doctor` before a hardware run. The stored
paths are the physical hub topology, not changing USB addresses or `ttyACM`
numbers. `all` verifies the generator's enrolled flash ID at the configured
generator path and the Core2350B's serial at the configured DUT path before
loading. If the boards were moved to different hub sockets, copy the doctor's
`configure-paths` command; otherwise do not change the local bench profile.

If C5 reports a D8 waveform error, first re-seat CH7/D6 on the shared D8/GP10
net. A floating CH7 produces many rapid transitions, whereas this C5 sequence
changes D8 only for its `0x0100` walking-bit transaction.

Run it with the stored W1 USB topology paths:

```sh
./run.sh all --output /tmp/c5-run-001
```

`all` builds, flash-loads both connected boards, waits for Enter, captures the
eight channels, validates waveform plus DUT evidence, and prints a report summary.
Use `./run.sh all --dry-run` to inspect the exact artifact and load plan. No
BOOTSEL press is needed for the normal C5 flash load.
