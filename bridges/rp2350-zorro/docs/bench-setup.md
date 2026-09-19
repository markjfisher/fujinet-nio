# Set up a generator bench on another computer

The current automated bench runner supports **Linux PCs with a compatible RP2040
board and an eight-channel fx2lafw analyzer**. It is not tied to a username, one
USB socket, or the original TZT board. Windows/macOS hardware discovery and serial
control are not implemented.

This document describes host and bench setup shared by the feasibility experiments.
See the feasibility index for currently implemented experiments.

## First setup

From the bridge directory:

1. Run `./scripts/setup.sh`. If you need the supplied Linux x86_64 compiler,
   use `./scripts/setup.sh --install-toolchain` instead. Other host architectures
   need a working Arm embedded compiler installed separately. See the
   [software setup guide](../README.md#reproducible-setup).
2. Install `sigrok-cli` and its fx2lafw firmware using your distribution's packages.
   Set up normal-user USB access using the
   [one-time access instructions](../tests/feasibility/README.md#one-time-linux-access-setup).
   The project rules contain no username, board serial or USB port number.
3. Wire the [W0 fixture](../tests/feasibility/generator-check/README.md#wiring-and-expected-result).
   GPIO numbers are logical chip pins; physical header positions depend on the board.
4. Run `./tests/feasibility/generator-check/run.sh build`, then run
   `./tests/feasibility/generator-check/run.sh configure` with the RP2040 in
   BOOTSEL to enroll its flash identity. Then run an implemented two-board
   experiment's `doctor` once and copy its `configure-paths` command to record
   the generator and DUT USB topology paths. Thereafter `run.sh all --output
   NEW_DIRECTORY` performs the normal build/load/run/analyse/report sequence.
   A separate prompt controls when the burst starts.

The board selection lives in `.bench/generator-check.json`, ignored by Git and
outside disposable build output. Version 1 profiles contain the generator flash
identity and analyzer. Version 2 profiles also contain the local generator and
DUT USB topology paths saved by `configure-paths`. A fresh checkout has no
inherited board choice or topology.
Use `--bench .bench/second-generator.json` for a separate bench profile when
running from the bridge directory. Only files under the bridge's `.bench/` are
automatically ignored; an arbitrary `--bench PATH` needs its own Git exclusion
if placed elsewhere in a checkout. Treat profiles as your local hardware
selections, not files to copy blindly to another person's bench.

For explicit enrollment without loading firmware, first run the starter's `build`
stage, then `configure`. To change boards, run `configure` again and confirm the
replacement. With multiple RP2040s in BOOTSEL, use the physical port shown by
`doctor` as `--usb-path`; the runner refuses an ambiguous choice. That port
selection is temporary and is not saved as the board's permanent identity.

After moving ports, run `configure-paths` again (or pass one-off `--usb-path`
and `--dut-usb-path` values). Rebooting the host requires a new `load` through
the full starter. A previous load session is deliberately not portable:
it binds the loaded image to the actual device instance on that host.

## Which values vary?

| Value | Meaning and portability |
| --- | --- |
| `2e8a:0003` | Raspberry Pi USB vendor ID plus RP2040 ROM BOOTSEL product ID. Shared by compatible RP2040 boards in that mode; unchanged by moving PCs. |
| `2e8a:000a` | RP2040 Pico SDK USB CDC identity used by this generator firmware. Shared, not unique to a board. Other firmware may use different IDs. |
| `2e8a:0009` | SDK USB CDC identity used for other supported chips, including the RP2350. It is not a permitted generator target. |
| Generator flash ID | Reported external-flash identity, normally different for each board. Selected during local enrollment, checked before every RAM load. A board must provide a usable identity. |
| `EEEEEEEEEEEEEEEE` | Placeholder runtime USB serial for this RAM image. Not accepted as a unique board identity. |
| USB bus/address, physical port, `ttyACM` number | Bus/address and `ttyACM` are dynamic. A physical topology path is stable while cabling stays put, so `configure-paths` records it locally; never copy one from another person's transcript. |
| Username/workspace path | Not fixed. Scripts locate their project; paths in build/session evidence describe that particular run. |
| Analyzer driver | Currently fx2lafw only. Default `fx2lafw` assumes one matching analyzer; use `--analyzer fx2lafw:conn=BUS.ADDRESS` to select among several for that run. Addresses can change. |
| GPIO2–5 data, GPIO6 /AS | Fixed generator firmware wiring. Different board headers are fine if they expose these GPIOs and meet the same electrical/clock assumptions. Editing a profile does not remap firmware pins. |
| Analyzer D0–D3 data, D7 /AS | Fixed capture mapping (physical CH1–CH4 and CH8 on the original unit). Other analyzer labels must be mapped to these sigrok channels. |
| 16 values, 100 us pulses, 300 us period, 1 MHz capture | Experiment requirements and oracle assumptions, shared across machines. Not personal preferences. |
| SDK `pico` board target | RP2040 generator clock/USB configuration. A board with incompatible clocks, USB wiring or unavailable GPIOs needs a reviewed firmware target change. |

The original bench's `754765170F445253` flash ID and RP2350 identity appear in
historical evidence, where they identify measured hardware. They are not default
target selectors for another user's checkout.

## What the USB permission rule does

`69-nio-feasibility.rules` grants the active local desktop user access to the two
RP2040 USB modes. It applies to those device classes, not one board, and does not
choose which board to load. Board selection and permission to access USB are
separate checks. Install the rule once per Linux host; it survives changing USB
ports and bus addresses. Reconnect after installing it.

The supplied `uaccess` rule assumes a systemd/logind local seat. On SSH-only,
headless or other Linux configurations, an administrator must arrange appropriate
device group/ACL access. Analyzer access comes from the distribution's sigrok
rules. No experiment command silently runs sudo or changes system policy.

This makes the existing fixture reproducible across compatible Linux benches.
It does not imply support for every operating system, MCU board or analyzer.
