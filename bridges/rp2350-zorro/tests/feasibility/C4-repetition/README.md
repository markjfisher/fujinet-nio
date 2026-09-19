# C4-repetition: repeated pulse and gap sweep

**C4 is a two-board W0 experiment.** It keeps the same four data wires and
`/AS` wire as C0–C3. Five distinct data values identify five groups of four
assertions, so the analyzer trace and Core2350B report identify the tested
condition without relying only on counters.

| Group | Data | Pulses | `/AS` low | Released gap between pulses |
| --- | ---: | ---: | ---: | ---: |
| low-100 | 1 | 4 | 100 us | 100 us |
| low-50 | 2 | 4 | 50 us | 100 us |
| low-20 | 3 | 4 | 20 us | 100 us |
| gap-50 | 4 | 4 | 100 us | 50 us |
| gap-20 | 5 | 4 | 100 us | 20 us |

C4 checks the 20 ordered captures and IRQs, each low width, and each
*intra-group* released gap. A longer high interval at a group boundary is
intentional: it contains the safe next-value transition and is retained as
trace evidence, not treated as a timing result. This establishes passing tested
points down to 20 us; it does not claim an RP2350 or Zorro timing limit, and it
does not deliberately seek a failure below that point.

Wiring is unchanged from W0: RP2040 GP2–GP5 to Core2350B GP2–GP5, RP2040 GP6
(`/AS`) to Core2350B GP1, shared ground, analyzer CH1–CH4 on D0–D3 and CH8 on
`/AS`. Keep all signals at 3.3 V. The existing local bench USB paths are reused.

```sh
./run.sh all --output /tmp/c4-run-001
```

`all` builds the host and firmware targets, force-loads both connected boards,
waits for Enter, captures the analyzer trace, checks the waveform and DUT
report, writes `waveform.svg`, and prints the report summary. Use `doctor` only
for setup/diagnosis; if cabling changes, run `configure-paths` again. C4 uses
APIO source, independent EPIO stimulus expectations, and EPIO DUT capture tests.
It introduces no ESP32 code, production ABI, or `.pio` source.
