# L1 — Arbitrary binary round-trip

Proves the candidate link is byte-transparent across deterministic binary payload families and representative packet lengths.

## Status

A reviewed physical pass is recorded in [the Story 2.3 evidence ledger](../../../docs/link-feasibility-evidence.md). `all` executes the 12 manifest-declared binary cases, records the Core2350B result for each one, captures the shared link wiring and writes an SVG evidence view. The `link_test_frame` is a feasibility-only SPI slot, never a production FujiBus ABI.

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

The ESP32-S3 GPIO numbers are the lab defaults for `esp32-s3-devkitc-1`; verify that they are safe on the actual breakout before wiring. Both boards use 3.3 V signaling and share ground.

## Build

```sh
./run.sh plan
./run.sh build
./run.sh all --output /tmp/l1-run-001
```

`build` configures and builds `link_rp2350` through the bridge CMake preset and builds the isolated ESP32-S3 PlatformIO project. It does not alter the product root `build.sh`, root PlatformIO configuration, or product firmware sources.

After the ESP32-S3 lab firmware has been uploaded once, `all` rebuilds both
targets, RAM-loads the Core2350B, runs the 12 cases and retains `capture.sr`,
`console.log`, `report.json` and `waveform.svg`. It uses the existing six-wire
connection; no wiring change from L0 is required.

## Profile

```json
{
  "packets": 12,
  "lengths": [
    0,
    1,
    16,
    64,
    240
  ],
  "patterns": [
    "zero",
    "ff",
    "increment",
    "alternating",
    "fixed-random",
    "slip-bytes"
  ]
}
```

Before physical loading, create the ignored local bench profile once from any experiment: 

```sh
./run.sh configure --rp-usb-path USB-TOPOLOGY --rp-port /dev/serial/by-id/RP2350 --esp-port /dev/serial/by-id/ESP32
```

The eventual physical runner will use that one local profile; no USB path belongs in this committed manifest.
