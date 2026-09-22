# L2 — Packet boundaries and size limits

L2 proves the limits of the 256-byte feasibility slot. It transfers empty,
minimal, near-maximum and maximum payloads, then verifies that an oversize
request is rejected locally and a deliberately truncated SPI slot is rejected
by the ESP endpoint. A delayed follow-up confirms recovery after the negative
case.

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

Both boards use 3.3 V signalling and share ground. This uses the unchanged W2
wiring from L0/L1. `CH1`–`CH6` correspond to analyzer `D0`–`D5`.

## Run

Configure the ignored local bench record once, as described in the parent
[README](../README.md), then run:

```sh
../../../lab/esp32-link/build.sh L2 --upload "$ESP_PORT"
./run.sh all --output /tmp/l2-run-001
```

Flash the selected ESP image whenever changing experiment. `all` builds both
endpoint images, RAM-loads the RP2350 without altering flash, arms the analyzer,
and runs eight manifest cases; it deliberately does not overwrite ESP flash. The report, raw `.sr` capture, console log and `waveform.svg` are
kept in the output directory.

The expected result is eight cases meeting their declared status: six normal
echoes, `oversize_rejected`, and `partial_rejected`.
