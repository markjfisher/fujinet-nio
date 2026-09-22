# L8 — Sustained performance

L8 measures the current lab fixture, not a production performance commitment.
The RP2350 performs a 3-by-3 payload/clock matrix, with three 50-transfer trials
per cell: 16, 64 and 240-byte payloads at requested 1, 4 and 8 MHz clocks. Each batch reports exact completed count, payload bytes, elapsed RP2350
microseconds and the baud rate accepted by the SPI peripheral. Batch frames
suppress per-slot ESP USB logs so the measured time is not console-paced; the
fixture holds READY low for a measured 500 us re-arm interval after each echo.

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

Use the unchanged W2 fixture with 3.3 V signalling and common ground. CH1–CH6
map to analyzer D0–D5. The 12 MHz analyzer is suitable for the 1/4 MHz traces;
it is diagnostic only at 8 MHz and does not certify edge timing.

## Run

```sh
./run.sh all --output /tmp/l8-run-001
```

The report retains every decoded analyzer transaction, the raw capture, both
endpoint consoles and image hashes. The SVG shows the first and last six
captured transactions for readability; it labels how many middle windows were
omitted from the drawing. `report.json` records per-cell minimum, mean and maximum payload rates. These
include the current two-slot exchange and READY re-arm behavior. The analyzer
is representative physical evidence only; its finite capture window does not
attempt to chart every batch. A batch failure is evidence, not a retry condition.
