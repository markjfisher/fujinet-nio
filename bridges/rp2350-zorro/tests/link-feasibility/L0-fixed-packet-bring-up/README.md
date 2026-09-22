# L0 — Fixed packet bring-up

Proves the SPI wires and test envelope transfer one fixed opaque packet and one echo without framing ambiguity.

## Status

A reviewed physical pass is recorded in [the Story 2.3 evidence ledger](../../../docs/link-feasibility-evidence.md): one 16-byte deterministic request and echo completed with analyzer evidence. The `link_test_frame` is a feasibility-only SPI slot, never a production FujiBus ABI.

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
./run.sh load-rp2350
./run.sh all --output /tmp/l0-run-001
```

`load-rp2350` force-reboots the single connected Core2350B into its ROM loader,
loads the no-flash lab image into SRAM and starts it. Its current flash firmware
is not replaced. Disconnect other RP-series targets before this step.

`build` configures and builds `link_rp2350` through the bridge CMake preset and builds the isolated ESP32-S3 PlatformIO project. It does not alter the product root `build.sh`, root PlatformIO configuration, or product firmware sources.

`all` is the normal repeatable L0 command after the ESP32-S3 lab image has been
uploaded once. It rebuilds both endpoint images, RAM-loads the Core2350B, records
CH1–CH6 at 12 MHz for 250 ms with sigrok, sends the fixed L0 transaction, and retains `capture.sr`,
`console.log` and `report.json` in the selected output directory. Close
PulseView's live capture first. The ESP32-S3 image is not reflashed by `all`.

## Profile

```json
{
  "packets": 1,
  "lengths": [
    16
  ],
  "patterns": [
    "increment"
  ]
}
```

Before physical loading, create the ignored local bench profile once from any experiment: 

```sh
./run.sh configure --rp-usb-path USB-TOPOLOGY --rp-port /dev/serial/by-id/RP2350 --esp-usb-path USB-TOPOLOGY --esp-port /dev/serial/by-id/ESP32
```

The eventual physical runner will use that one local profile; no USB path belongs in this committed manifest.
