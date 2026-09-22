# Story 2.3 link feasibility experiments

For the endpoint roles, build/load flow, fixed test slot, and retained evidence,
see [the link-lab implementation guide](IMPLEMENTATION.md). The
[Story 2.3 plan](../../docs/story-2-3-link-feasibility-plan.md) remains the
authority for each experiment's purpose and acceptance evidence.

These are isolated RP2350B ↔ ESP32-S3 laboratory targets. They use a fixed-size,
test-only SPI slot (`link_test_frame`) with a sequence, length, payload and CRC16.
It is a measurement envelope, not a FujiBus packet format or candidate production
ABI. The payload is opaque to the RP2350 transport test.

The RP2350 target is built with the bridge's pinned Pico SDK:

```sh
cmake --preset link-rp2350
cmake --build --preset link-rp2350 --target link_rp2350
```

The ESP32-S3 target is a dedicated PlatformIO project:

```sh
lab/esp32-link/build.sh L0
# With the ESP32-S3 in its bootloader, after selecting a stable serial path:
lab/esp32-link/build.sh L0 --upload /dev/serial/by-id/ESP32-S3-PORT
```

It uses the same ESP-IDF PlatformIO platform version as the FujiNet ESP32-S3
build but never invokes or modifies the root product `build.sh`, root
`platformio.ini`, `platformio.local.ini`, or product sources.

Each L0–L9 directory owns a manifest, run wrapper and hardware README. Start with:

```sh
cd tests/link-feasibility/L0-fixed-packet-bring-up
./run.sh plan
./run.sh build
./run.sh doctor
```

Each manifest uses the same provisional W2 mapping: RP2350 GP2..7 map to ESP32-S3
GPIO12,11,13,10,9,8 for SCLK, MOSI, MISO, CS, READY and DATA_AVAILABLE.
The particular ESP32-S3 breakout must be checked before wiring. Save both
serial ports and physical USB topologies once in ignored
`.bench/link-feasibility.json` using `./run.sh configure ...`; see
`bench.example.json`.

The observed ESP32-S3 USB device is `303a:4002` at topology `7-1.3.3.4.3`,
with serial string `123456` and two CDC interfaces. The serial string is not a
unique board identity. Do not enrol `ttyACM0` or `ttyACM1` by number: flash the
lab endpoint, identify the interface that prints its `ready` line, then record
its stable `/dev/serial/by-id/...` path as `esp_port`.

On Linux, install the shared feasibility udev rule once before the first ESP32
upload. It grants the active local desktop user access to Espressif USB
Serial/JTAG bootloader `303a:1001` and USB-device `303a:4002`, as well as the
existing Raspberry Pi lab devices:

```sh
cd ../feasibility
sudo install -m 0644 69-nio-feasibility.rules /etc/udev/rules.d/69-nio-feasibility.rules
sudo udevadm control --reload-rules
```

Unplug and reconnect the ESP32-S3 afterwards. The rule grants port access only;
it does not choose a board or upload an image.

The initial firmware supports a serial `run [scenario] [length] [pattern]` command
on RP2350 USB, where patterns are zero, FF, increment, AA/55, fixed random and
SLIP-relevant bytes. The ESP32 slave validates one whole slot and returns it on the
next slot. This proves only the initial L0 link mechanism; L1–L9 manifests define
the sequences, fault injections and evidence each later physical runner must
record. Reset and disconnect injections remain manual lab actions until those
individual runners are implemented and validated on the actual boards.
