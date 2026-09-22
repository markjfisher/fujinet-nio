# L5 — Controlled bidirectional scheduling

L5 proves a declared full-duplex lab schedule. For each case, the ESP32-S3 has
already queued one autonomous frame. The RP2350 sends its request while
validating that ESP frame on MISO in the same slot, then clocks a second slot
to validate the request echo. The named schedules document the intended order
of work; they do not define a production ownership policy.

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

Use W2 unchanged, with common ground and 3.3 V signalling. `CH1`–`CH6` map to
analyzer `D0`–`D5`.

## Run

L5 needs its ESP boot queue, so upload that endpoint image first:

```sh
../../../lab/esp32-link/build.sh L5 --upload "$ESP_PORT"
./run.sh all --output /tmp/l5-run-001
```

Reflash or reset the ESP32 before rerunning, so its autonomous sequence begins
at frame 1. A pass means each case validated both the ESP-originated frame and
the RP-originated echo. The evidence directory contains `report.json`, console
output, raw analyzer samples and `waveform.svg`.
