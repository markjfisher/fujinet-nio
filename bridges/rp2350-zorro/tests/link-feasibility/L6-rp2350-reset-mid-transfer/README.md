# L6 — RP2350 reset after partial transfer

L6 is an automated reset-containment experiment. The RP2350 clocks 32 bytes of
a 64-byte frame and deliberately leaves the ESP32-S3 error response pending.
The runner closes the RP console, uses picotool to force-reset and RAM-reload
the Core2350B, then sends a fresh 64-byte frame. A pass proves the recovered
master drains the stale rejection rather than attributing it to the new request.

This covers reset **after a partial SPI slot and before completion is consumed**.
It does not claim to reset the RP2350 at an arbitrary clock edge; that requires
a separate physical reset-timing fixture.

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
analyzer D0–D5. Keep the intended Core2350B connected: `all` force-loads its
SRAM image during the fault phase; it does not write flash.

## Run

```sh
./run.sh all --output /tmp/l6-run-001
```

Expected case results are `partial_injected`, `rp_reset_reloaded`, and `passed`.
The SVG records the physical slot activity captured during its finite acquisition
window; the RP result lines are the authoritative evidence of the reload and
fresh recovery exchange.
