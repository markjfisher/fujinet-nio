# L3 — Receiver pressure and backpressure

L3 proves that the `READY` line holds the RP2350 until the ESP32 endpoint has
finished bounded simulated receiver work. It runs fixed payloads through
0, 1, 10 and 100 ms pauses, with declared queue-depth settings, then verifies
a normal transfer still works after the longest pause.

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

Use the unchanged W2 wiring and 3.3 V logic levels. `CH1`–`CH6` map to
analyzer `D0`–`D5`.

## Run

Flash the L3 ESP image once after this source change, then use the repeatable
runner:

```sh
lab/esp32-link/build.sh L3 --upload "$ESP_PORT"
./run.sh all --output /tmp/l3-run-001
```

The output directory contains the endpoint console, analyzer capture,
`report.json`, and `waveform.svg`. A pass means every pause profile produced
its expected echo and the final recovery case passed. The queue-depth field is
explicit lab control data, not a production protocol field.
