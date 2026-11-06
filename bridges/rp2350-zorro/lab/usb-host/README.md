# USB host bench probe

First software slice of the workspace's `backlog/amiga-zorro-usb-bringup.md`,
deliverable 1. This is an isolated bench image for a **Waveshare Core2350B**.
Confirm the board before loading it. Keep the Amiga and all old SPI/bus wiring
disconnected. Hardware acceptance remains pending physical runs.

## Components and wiring prerequisites

- RP2350 board, its native USB breakout, and BOOTSEL/reset access.
- ESP32-S3 board, native USB breakout (not its USB-to-UART connector), and
  BOOT/reset access. Confirm model, flash and PSRAM before selecting its image.
- One regulated 5 V supply with sufficient current for both boards; confirm
  each board's power schematic and breakout pin labels before joining power.
- Short USB data wiring: D+ to D+, D- to D-, common GND. Keep D+/D- together
  and short, preferably a USB cable pair. Avoid long breadboard paths or stubs.
- A 3.3 V logic USB-UART adapter/debug probe for the RP console: adapter RX to
  RP GP0/TX, adapter TX to RP GP1/RX, GND to GND. **Leave adapter VCC/5V/3V3
  disconnected.** These are temporary bench GPIOs, not Zorro pin allocations.
- A multimeter for continuity, supply voltage and isolation checks.

No external LED is required: the image uses the Core2350B onboard GP39 LED.
Do not attach LEDs, pull resistors or test loads to D+/D-. Optional rail LEDs
need their own series resistors and do not prove USB enumeration or link health.
If an adapter can drive TX while the RP is unpowered, disconnect its signal
wires during power-off/recovery changes to avoid phantom powering.

Start with manually moved jumpers, with all power removed between modes.
Treat this as a patch panel, not a live USB switch. The eventual switch and
its signal integrity will require their own qualification.

| Mode | D+/D- connection | Other USB data connection |
| --- | --- | --- |
| Run | RP native USB to ESP native USB | External PC data disconnected from both |
| Flash RP | Shared PC connector to RP native USB | ESP isolated |
| Flash ESP | Shared PC connector to ESP native USB | RP isolated |

**The power wiring is board-specific and must be resolved before connection.**
Do not simply join the boards' VBUS/5 V rails while either has another USB
cable or supply attached. In Run mode, there must be an intentional VBUS
source for the ESP link. In flashing modes, PC VBUS must not be driven by the
bench supply. Do not join the boards' regulated 3.3 V outputs. Verify USB-C
breakout CC handling if the shared connector is USB-C. A generic DPDT switch
alone does not address power routing or USB-C attachment.

## Confirmed ESP-Prog console setup

Bench observation supplied by Mark on 2026-11-06: the official Micro-USB
ESP-Prog enumerates as an FT2232H with two interfaces. It connects to the
computer through Micro-USB and to the RP through its **6-pin PROG header**;
it does not connect to the ESP32-S3 USB socket. The 10-pin JTAG header is unused.

| Linux by-id path under `/dev/serial/by-id/` | Observed device | Use |
| --- | --- | --- |
| `usb-FTDI_Dual_RS232-HS-if00-port0` | `/dev/ttyUSB1` | ESP-Prog JTAG channel; not the console |
| `usb-FTDI_Dual_RS232-HS-if01-port0` | `/dev/ttyUSB3` | ESP-Prog PROG UART; RP console |
| `usb-Prolific_Technology_Inc._USB-Serial_Controller_D-if00-port0` | `/dev/ttyUSB0` | Separate adapter; not used for this console |

Use this console path rather than the transient `ttyUSB` number:

```sh
RP_CONSOLE=/dev/serial/by-id/usb-FTDI_Dual_RS232-HS-if01-port0
```

Both ESP-Prog device nodes were owned by `root:uucp` with mode `0660`; Mark is
already a member of `uucp`. No permission change is required. The by-id name
has no unique serial suffix, so distinguish by USB topology or a programmed
serial number if another identical FTDI adapter is attached later.

| ESP-Prog PROG pin | RP connection |
| --- | --- |
| 3, `ESP_TXD` | GP0, RP UART TX |
| 5, `ESP_RXD` | GP1, RP UART RX |
| 4, `GND` | Common ground |
| 1 (`ESP_EN`), 2 (`VDD`), 6 (`ESP_IO0`) | Leave disconnected |

These labels describe the target's UART directions: pin 3 receives into the
ESP-Prog and pin 5 transmits from it. See the
[official schematic](https://dl.espressif.com/dl/schematics/SCH_ESP32-PROG_V2.1_20190709.pdf).
Locate pin 1 using the board marking before wiring with power disconnected.
Leave the jumpers unchanged for this three-wire console connection. Actual
RP console traffic has not yet been verified.

### Existing local udev aliases

Mark's `/etc/udev/rules.d/99-fujinet-nio.rules` already contains the following
tty aliases, all matching `ACTION=="add"` and `SUBSYSTEM=="tty"`. This is a
record of the supplied configuration, not an installed or modified rule file.

| Kernel name | Vendor ID | Product ID | USB interface | Symlink under `/dev/` |
| --- | --- | --- | --- | --- |
| `ttyACM*` | `303a` | `4002` | `00` | `fujinet-fujibus` |
| `ttyACM*` | `303a` | `4002` | `02` | `fujinet-console` |
| `ttyACM*` | `303a` | `4001` | `00` | `fujinet-fujibus` |
| `ttyACM*` | `303a` | `1001` | `00` | `fujinet-rs232` |
| `ttyUSB*` | `10c4` | `ea60` | `00` | `fujinet-atari` |

The RP console currently uses the FTDI by-id path above; no additional alias
is needed. A future `/dev/fujinet-rp-console` alias should match the actual
ESP-Prog udev properties and interface `01`, not a `ttyUSB` number. These ESP
runtime aliases do not identify the ESP ROM flashing port. In Run mode the
RP owns the ESP USB link, so its CDC ports are not available directly on the PC.

## Build and load the RP

From `bridges/rp2350-zorro`:

```sh
./scripts/build.sh usb-host-rp2350
```

The script sources the workspace environment and validates pinned dependencies.
It builds only `usb_host_probe`, using native USB host support from the pinned
Pico SDK/TinyUSB and the Springboard C PacketLink controller directly.
Output: `build/usb-host-rp2350/usb_host_probe.uf2` (and `.elf`).

Select Flash RP wiring with power off. Hold BOOTSEL while connecting the PC,
then copy the UF2 to the RP boot drive. Verify the drive belongs to the RP
under test. The running image is a **USB host**, so the PC console/device
disappearing after flashing is expected. Disconnect power, select Run wiring,
and power up using the reviewed single-source arrangement.

Physical BOOTSEL remains the recovery path even with broken firmware. The
`bootsel` console command is a convenience, not the recovery proof. Do not
rely on `picotool -f` against the running host image.

## ESP software

Use NIO's USB CDC **PacketLink** profile, not its SLIP USB profile. The bench
environment is `fujibus-link-usbcdc-bench-s3-n16r8`: 16 MB flash, 8 MB octal
PSRAM, and an empty application pin map. It does not initialize SD/bus/LED/button
GPIOs. This deliberately overrides the underlying board JSON's default SD map.

In a disposable firmware checkout/copy, from its root, run:

```sh
python3 pio-build/scripts/create-platformio-ini.py -n fujibus-link-usbcdc-bench-s3-n16r8
# Set console_type = consolecdc in that copy's platformio.local.ini.
python3 pio-build/scripts/create-platformio-ini.py
touch sdkconfig.local.defaults
python3 pio-build/scripts/create-sdkconfig.py -o sdkconfig.defaults \
  -m pio-build/sdkconfig/00_platform_sdkconfig_map.txt -i platformio.ini
pio run
```

Source the workspace environment first. The copy needs the populated
`third_party/yaml-cpp`, `third_party/cjson` and `third_party/esp/esp_littlefs`
dependencies. The config generators replace local config files; use the copy
to preserve another active board setup. Build output is under
`.pio/build/fujibus-link-usbcdc-bench-s3-n16r8/`.

For flashing, select Flash ESP wiring, hold BOOT while tapping RST, then
release BOOT. Select the newly enumerated native USB serial port explicitly:

```sh
pio run -t upload --upload-port /dev/serial/by-id/REPLACE_WITH_ESP_ROM_PORT
```

This uploads the bootloader, partition table and application at the environment's
correct offsets. Do not flash the application binary alone at address zero.
Do not erase or upload a filesystem for the clock test. An existing stored
configuration may affect clock/network state; preserve it and inspect the
diagnostics console before changing it.

No SD card or external SD adapter is required. The bench pin map disables all
SD pins, and startup skips the SD driver when the mapping is incomplete.
Internal LittleFS mounts the `storage` partition and formats it automatically
if mounting fails; a separate filesystem upload is unnecessary for the console
and clock test. Packaged boot images are not required for either test.

The probe expects CDC0/control-interface 0 for packets and CDC1/control-interface
2 for logs. It asserts DTR/RTS on enumeration and drains logs separately, counting
their bytes without feeding them into PacketLink. It uses USB-OTG CDC in Run
mode; native ROM flashing has a different USB identity.

## Console and first run

Open the separate UART at **115200, 8N1, no flow control**. Send newline-ended
commands; there is no local echo. On boot, the banner identifies the host probe.
On enumeration it reports both CDC interfaces, synchronizes PacketLink, then
issues one read-only `Clock.GetTime` request. There is no continuous request loop.

| Command | Behaviour |
| --- | --- |
| `status` | Link state, interface indices, generation, capacity, pending transmit bytes, fault and protocol counters |
| `clock` | A new Clock.GetTime, only while synchronized and idle |
| `send HEX` | One raw FujiBus request, contiguous hex, up to 8192 local bytes and within the peer's advertised capacity; validates length/checksum |
| `resync` | Discard local pending work, clear USB FIFOs and establish a fresh generation; does not replay the previous request |
| `bootsel` | Enter ROM USB device mode; power off and select Flash RP wiring before connecting the PC |

For example, `send 450106004c00` sends the same clock request without the
special clock decoder. Arbitrary `send` commands can have service side effects;
use read-only requests or disposable test media.

The clock decoder verifies the raw checksum, device/command, status descriptor
and v1 time payload. `status=0` carries Unix seconds; a valid nonzero service
status proves a response but does not prove that the ESP clock is initialized.
Raw responses report size, CRC32 and a short prefix; that is not a full
known-data comparison for bulk-transfer acceptance.

LED meanings: slow blink while waiting; steady after a structurally valid
response (including a service error); fast blink for a latched fault. UART
output is the detailed record. Answer timeout is 3 seconds; stalled local USB
transmit progress is limited to 1 second. Unknown completion latches a fault;
use `resync` and submit a **new** request deliberately. The reused controller
can retry definite `Corrupt`/`NotSynchronised` rejections; it never retries an
unknown completion. The `resends` counter makes those retries visible.

## Checks and evidence

From the workspace, source the environment, then run these from the bridge:

```sh
source "$NIO_WORKSPACE/scripts/env.sh"
cmake --preset host
cmake --build --preset host --target test_usb_probe test_usb_packet_link
ctest --preset host -R '^usb_'
./scripts/build.sh usb-host-rp2350
```

Native tests exercise bounded queued writes including short writes, temporary
backpressure and wraparound; literal clock responses; and the reused controller's
framing, generations and recovery. They do not exercise the physical USB host
controller, GPIOs, USB switching, power paths or enumeration.

Next physical sequence: verify wiring/power, flash each board independently,
Run/Clock, reset/disconnect during an exchange, resynchronize and issue a new
Clock. Save UART output with image hashes and a wiring photo/diagram. Complete
the workspace checklist's maximum-size data verification, partial-record/stall
faults, repeated recovery and measured throughput before accepting deliverable 1.
