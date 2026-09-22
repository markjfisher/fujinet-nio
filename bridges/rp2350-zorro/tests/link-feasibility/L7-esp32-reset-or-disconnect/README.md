# L7 — ESP32 reset and recovery

L7 requests a feasibility-only ESP32-S3 restart after a completed control slot.
The RP2350 must observe both `READY` and `DATA_AVAILABLE` withdraw, while the
runner waits for the ESP USB console to re-enumerate. It then sends a fresh
64-byte request which must complete without replaying the pre-reset control
frame.

The reset is deliberately at a slot boundary, where the experiment can make an
unambiguous oracle. It is not a test of an arbitrary power cut or disconnect in
the middle of SCLK; those physical fault phases remain follow-up work.

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

Use W2 unchanged with common ground and 3.3 V signalling. CH1–CH6 map to
analyzer D0–D5. The ESP reset drops its USB serial interface briefly; do not
hold another terminal open on the enrolled ESP port while running the test.

## Run

```sh
./run.sh all --output /tmp/l7-run-001
```

Expected results are `peer_reset_detected` followed by `passed`. `all` uploads
the L7 ESP image before the run, so its restart returns to the same lab image.
