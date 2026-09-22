# L4 — Reverse independent ESP-to-RP transfer

L4 proves that the ESP32-S3 can schedule test frames before any RP2350 request.
After boot, the ESP prepares sixteen deterministic frames and asserts
`DATA_AVAILABLE`; the RP2350 only clocks them out and validates their sequence,
length, CRC, and payload. The six-case profile samples the first two cycles of
1, 64 and 240-byte frames.

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

W2 is unchanged. Use 3.3 V signalling with one shared ground; `CH1`–`CH6`
correspond to analyzer `D0`–`D5`.

## Run

L4 has an ESP-originated boot queue. `all` RAM-loads the RP2350 first, then
restarts the ESP image. This starts the autonomous queue at frame 1 after the
RP2350 SPI pins have settled:

```sh
./run.sh all --output /tmp/l4-run-001
```

Each `all` run reuploads and waits for the ESP image, restoring its autonomous
sequence at frame 1. `all` builds both endpoints, RAM-loads the RP2350 and
records the console, capture, `report.json`, and `waveform.svg`.
