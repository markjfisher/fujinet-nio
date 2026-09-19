# C6-pressure: bounded FIFO stall and recovery

C6 deliberately proves the current PIO-to-ARM limitation before any production
transfer design is chosen. It uses the existing W1 wiring and the same APIO W1
capture loop as C5: PIO0 SM0 waits for `/AS`, snapshots all 21 W1 inputs,
performs `PUSH BLOCK`, raises PIO IRQ 0, then waits for `/AS` to release. The
ARM observer is deliberately paused for 2 ms when its first RX record appears.
The generator is never slowed to conceal that pause.

The generator emits sixteen selected writes (`0x1000` through `0x100f`) at the
normal C5 cadence. `PUSH BLOCK` retains four FIFO words and the fifth sampled
ISR word, then stalls the state machine. The next eleven assertions occur while
the state machine is stalled and are explicitly reported as unobserved; they are
not overwritten or presented as completions. Ten released W1 state pairs give
the observer time to resume. Four sentinel writes (`0xd001` through `0xd004`)
then prove that capture resumes on the same state machine.

A successful DUT report is therefore intentionally **not** a 20-capture result:

```text
capture_count=9 capture_irq_count=9 raw_capture_count=9
pressure_pause_count=1 pressure_pause_us=2000
pressure_expected_assertions=20 unobserved_assertion_count=11
values=4096,4097,4098,4099,4100,53249,53250,53251,53252
```

`unobserved_assertion_count` is the declared finite stimulus assertion count
less the raw PIO records. It is a reconciliation result, not an invented FIFO
overflow flag. The generated `waveform.svg` proves all twenty observed W1
assertions and their control/data subset; the DUT report proves which complete
words reached PIO RX FIFO and ARM RAM. It remains an 8-channel subset trace:
the full 16-bit values are independently checked from the DUT report.

The focused EPIO test proves APIO program assembly, FIFO fill and the blocking
state. Pinned EPIO does not document resuming a `PUSH BLOCK` state after a host
FIFO pop, so the live recovery transition is deliberately a physical C6 oracle;
the test does not fabricate an emulator recovery claim.

Connect analyzer ground to W1 common ground and leave its CLK input open.
No W1 rewiring is required after C5.

| Analyzer channel | W1 net | RP2040 / Core2350B GPIO |
| --- | --- | --- |
| CH1 / Sigrok D0 | `/AS` | GP18 |
| CH2 / Sigrok D1 | `SELECT` | GP22 |
| CH3 / Sigrok D2 | `R/W` | GP19 |
| CH4 / Sigrok D3 | `/UDS-like` | GP20 |
| CH5 / Sigrok D4 | `/LDS-like` | GP21 |
| CH6 / Sigrok D5 | D0 | GP2 |
| CH7 / Sigrok D6 | D8 | GP10 |
| CH8 / Sigrok D7 | D15 | GP17 |

Run C6 with the saved W1 USB topology paths:

```sh
./run.sh all --output /tmp/c6-run-001
```

`all` builds the C6 generator and fault-injection DUT presets, flash-loads both
boards, waits for Enter, captures the analyzer trace, validates waveform and
DUT reconciliation, then writes `report.json` and `waveform.svg`. No BOOTSEL
button is required. Use `./run.sh all --dry-run` to inspect the exact plan.

This establishes the blocking-push stall behavior of this bounded polling
baseline. It does not claim sustained throughput, an NVIC/DMA drain solution,
or a Zorro-II timing margin; those are the next C6 variants and later work.
