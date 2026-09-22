# L9 — Recovery soak

L9 repeatedly proves recovery from a known incomplete-slot fault. It sends 100
exact 64-byte transfers. Before every tenth transfer, the RP2350 deliberately
clocks only 32 bytes of a test frame and leaves the resulting peer rejection
advertised. The following normal transfer must drain that stale error, then
complete byte-for-byte correctly. The final result reports completed cycles,
injected fault count and RP2350 elapsed microseconds.

This is the automated **partial-transfer recovery** soak. It does not claim to
cover physical power removal, USB unplug, or reset of either board; L6 and L7
own those separate reset procedures.

## Wiring

| Signal | Core2350B RP2350 | ESP32-S3 lab default | Analyzer |
| --- | --- | --- | --- |
| GND | GND | GND | GND |
| SCLK | GP2 | GPIO12 | CH1 |
| MOSI | GP3 | GPIO11 | CH2 |
| MISO | GP4 | GPIO13 | CH3 |
| CS | GP5 | GPIO10 | CH4 |
| READY | GP6 input | GPIO9 output | CH5 |
| DATA_AVAILABLE | GP7 input | GPIO8 output | CH6 |

Use W2 unchanged with 3.3 V signalling and one shared ground. CH1–CH6 map to
analyzer D0–D5.

## Run

```sh
./run.sh all --output /tmp/l9-run-001
```

A pass is `soak_pass cycles=100 injected_faults=9`. Retain the report and both
endpoint transcripts. Do not treat a pass as reset/disconnect evidence.
