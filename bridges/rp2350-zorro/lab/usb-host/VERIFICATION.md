# USB bench software verification

2026-11-06: initial bench software built; no physical USB results claimed.

The workspace environment was sourced before each check. From the bridge:

| Command | Result |
| --- | --- |
| `cmake --preset host` | Passed |
| `cmake --build --preset host --target test_usb_probe test_usb_packet_link` | Passed |
| `ctest --preset host -R '^(usb_probe\|usb_packet_link\|setup\|tooling\|pio_policy)$'` | 5 passed |
| `cmake --preset host-release` | Passed |
| `cmake --build --preset host-release --target test_usb_probe test_usb_packet_link` | Passed |
| `ctest --preset host-release -R '^usb_'` | 2 passed |
| `./scripts/build.sh usb-host-rp2350` | Passed, ELF and UF2 generated |

ESP: the README's config generation and `pio run` passed in an isolated copy
at `/tmp/zorro-usb-esp-build`, with populated repository dependencies and
`console_type = consolecdc`. The main working tree's Atari configuration was
preserved. Generated configuration confirmed `CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`,
`CONFIG_SPIRAM_MODE_OCT=y`, two CDC interfaces, FujiBus on port 0, console/logs
on port 1. Firmware, bootloader and partition binaries were copied to
`build/usb-host-esp/`; hashes are in its `SHA256SUMS`.

Workspace/local document links, code-fence balance and `git diff --check`
passed. The RP build used the existing pinned Pico SDK 2.3.1 and Arm GNU
14.2.Rel1 toolchain. The ESP environment uses espressif32 6.10.0.

## First RP flash observation

On 2026-11-06 Mark reported entering BOOTSEL by power-cycling with BOOT held,
then copying `build/usb-host-rp2350/usb_host_probe.uf2` to the RP boot drive.
The drive contents disappeared and the onboard LED began flashing. This is
consistent with the image starting and waiting for its USB peer; blink cadence
and UART status have not yet been captured. It does not establish RP-to-ESP
communication or shared-connector recovery.

A USB-C breadboard power adapter has been ordered, advertised with selectable
3.3 V / 5 V outputs and 1 / 1.5 A ratings. The exact model, rating per output,
rail selection and measured voltage remain to be checked on arrival. The
planned Run supply is 5 V to RP VBUS and ESP 5V, with common ground; ESP-Prog
receives power from PC USB and its VDD header remains disconnected.

Hardware still to verify: USB enumeration and interface selection, UART output,
full LED-state behaviour, independent ROM recovery, real clock response,
maximum-size known-data transfers, fault injection, power/mode switching and
measured rates. Build success and this initial flash observation do not close
those checklist items.

## ESP standalone console investigation

On 2026-11-06 Mark flashed the bench profile with `console_type = consolecdc`
and `FN_DEBUG`. Runtime USB enumerated as 303a:4002, with interface 00 on
`/dev/fujinet-fujibus` (`ttyACM0`) and interface 02 on `/dev/fujinet-console`
(`ttyACM1`). USB remained connected until an explicit reset, but the console
showed neither input echo nor command responses. A roughly five-second LED
flash was observed; its cause is unknown, and the bench pin map defines no
application LEDs. No SD hardware is connected and no filesystem was uploaded.

Source inspection confirmed LittleFS formats on mount failure. SD initialization
previously passed the disabled pin mapping to the driver; it now returns before
any SPI initialization when a required pin is absent. This is not a confirmed
explanation for the console symptom.

With the workspace environment sourced, `./build.sh -b` in the firmware root
passed for `fujibus-link-usbcdc-bench-s3-n16r8`, preserving the user's console and
debug settings. `git diff --check` passed. Hardware verification of the SD skip
and console response remains pending: serial devices are not exposed in the
agent environment. Next check: flash this application build, reconnect the
monitor to `/dev/fujinet-console`, and try `help` and `core.info`.

Mark subsequently reported that reflashing the SD guard still left the console
silent with the same roughly five-second LED activity. Added a temporary bench
`FN_DEBUG_BOOT_COUNTDOWN`: five-second initial delay, direct CDC `booting up`,
five one-second countdown lines, then `starting application`, all before log
redirection and filesystem initialization. With the workspace environment
sourced, `./build.sh -b` passed; both marker strings were confirmed in the built
ELF and `git diff --check` passed. Countdown output/timing on hardware is pending
the next flash; this build does not establish the cause of the silent console.
