# L8 — Sustained performance

Measures successful transfer rate and latency over sustained deterministic traffic at selected SPI clock rates.

## Status

The shared endpoint firmware and build runner are implemented. Hardware execution remains pending: this experiment must record endpoint console output and an analyzer capture before it can claim a pass. The `link_test_frame` is a feasibility-only SPI slot, never a production FujiBus ABI.

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
```

`build` configures and builds `link_rp2350` through the bridge CMake preset and builds the isolated ESP32-S3 PlatformIO project. It does not alter the product root `build.sh`, root PlatformIO configuration, or product firmware sources.

## Profile

```json
{
  "packets": 1000,
  "lengths": [
    16,
    64,
    240
  ],
  "spi_hz": [
    1000000,
    4000000,
    8000000
  ]
}
```

Before physical loading, create the ignored local bench profile once from any experiment: 

```sh
./run.sh configure --rp-usb-path USB-TOPOLOGY --rp-port /dev/serial/by-id/RP2350 --esp-port /dev/serial/by-id/ESP32
```

The eventual physical runner will use that one local profile; no USB path belongs in this committed manifest.
