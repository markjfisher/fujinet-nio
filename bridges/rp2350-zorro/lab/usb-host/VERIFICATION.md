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

Hardware still to verify: USB enumeration and interface selection, actual UART
and LED behaviour, independent ROM recovery, real clock response, maximum-size
known-data transfers, fault injection, power/mode switching and measured rates.
Build success does not close these checklist items.
