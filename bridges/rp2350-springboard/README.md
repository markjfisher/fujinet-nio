# RP2350 Springboard bridge

This independent project is firmware for a Handspring Visor Springboard module
built from a Waveshare Core2350B (RP2350B, `waveshare_core2350b`,
`rp2350-arm-s`) on mozzwald's Springboard carrier. The module does two things
on the Visor's expansion bus:

- it serves the module ROM (the Palm apps) on the first chip select, CS0;
- it provides a two-way link on the second, CS1, which carries the Palm's
  FujiBus packets to NIO over a USB serial port using NIO's
  [packet link](../../docs/packet_link.md).

```
Palm app  --Springboard CS0/CS1-->  RP2350  --USB CDC, packet link-->  NIO (POSIX build)
          ROM / two-way link        this firmware      raw FujiBus      fujibus-link-serial-debug
```

It is not a Palm toolchain or a ROM: the Springboard ROM image (Palm apps plus
the Handspring card header) is built elsewhere and handed to this build.
It does not drive the Springboard `/IRQ` line (GP0), and it is not NIO on the
module: the firmware is a USB device, so NIO runs on a computer attached to the
board. An ESP32 acting as USB host is future work.
The production firmware root build does not include this tree.

The CS1 register map shared with the Palm side is
[`include/sbnio.h`](include/sbnio.h); [docs/sbnio-link.md](docs/sbnio-link.md)
describes the link, its rules and the NIO side.

## Credit

The bus engineering here is mozzwald's. This bridge stands on:

- **The carrier board and its pinout**: the Springboard carrier for the
  Core2350B, with the data bus contiguous on GP20-35, the controls on GP16-19
  and GP39/GP47 kept off the bus.
- **The bus research**: weeks of bring-up on a real Visor Prism, with passive
  captures, scope and logic-analyzer timing, the read-cancellation and
  paired-read work and the 330 ns CS0 cycle measurements. The first CS1 write
  captures showed that a second state machine can sample the bus.
- **The read engine**: the persistent PIO and Core1 responder that answers
  every read ("Experiment 2", dual-select) is his hardware-proven code from
  [mozzwald/fujinet-nio](https://github.com/mozzwald/fujinet-nio), branch
  `experiment/dual-cs-write-capture`, `bridges/rp2350-springboard`.

| Here | From mozzwald, and what changed |
| --- | --- |
| `src/pio_rearm_program.c`, `.h` | the PIO read program; a provenance comment only |
| `src/persistent_reply.S`, `.h`, `src/cpu_reply.h` | Core1's reply loop. Changed: it runs from SCRATCH_X, and a CS1 read looks up the register table that follows the ROM table (his returned a constant) |
| Core1 service loop in `src/bus.c` | from his `msc_main.c`; the CS1 reply source is the only change |
| `tests/test_pio_rearm.c` | his epio test of the read program, with CS1 reads and write cycles added |

His four source files are listed in `.clang-format-ignore` so they stay as he
wrote them. The rest of the firmware builds on them: the CS1 write capture
(one GPIO16-47 snapshot per write on PIO2, DMA into a ring), the register
table and request/reply protocol, the packet-link controller and the USB and
console code.

## Status

Hardware: a Visor Prism (Palm OS 3.5), a Core2350B on the carrier, NIO on a
Mac. Everything below was checked by hand; none of it runs in CI.

Verified on the Prism:

- Module ROM on CS0: the Prism recognises the module and lists its apps, which
  run in place from the ROM. The apps ask for a 450 ns access time and the
  Prism reports 330 ns, which works.
- Two-way CS1 link: 512 words written and read back, 512/512 in 90 ms; 17
  requests of 1-4096 bytes looped back by the firmware (25,708 bytes each way
  in 260 ms, about 99 KB/s each way); 14,725 CS1 writes with no check failure.
  WE stays low 340-406 ns per write and the firmware samples 53 ns after it
  falls.
- Through NIO over the packet link, against the `fujibus-link-serial-debug`
  build: `Clock.GetTime` and `GetTimezone`; an HTTP GET (200, the 577-byte
  page read in 240 ms); an HTTPS GET (200, 370 ms); two JSON queries answered by
  NIO's translator (5.0 s, most of it waiting on the remote server). 111
  requests in one generation, all answered, each in 0-1 ms at the firmware;
  no resends, corrupt records or timeouts. 15,095 CS1 writes in that run with
  no check failure.

Not verified: other Visor models; hot insertion and removal; CS0 access times
other than the Prism's; NIO restarting under a running Palm app; long-term
reliability; the placeholder ROM on a Visor.

## Safety

The Springboard connector is the Visor's own bus; treat the first insertion of
any module as an experiment.

- Check the carrier against the pin table below before inserting it. A missing
  joint on /CS1 (GP17) cost a bring-up session; the `float` console command
  shows which lines the Visor is driving.
- The firmware drives only D0-D15 (GP20-35) and the LED (GP39). PIO1 enables
  the data pads for a qualified read only (CS low, OE low, WE high, RESET
  high) and releases them through state machines that do not depend on the
  CPU when the select or OE ends or RESET falls. At start the firmware waits
  for CS0 and OE released and RESET high before taking the bus. The write
  capture, the select counters and the WE timer on PIO2 are input-only. Every
  other pin is an input with its pulls off, including GP0 (/IRQ). GP47 (the
  Core2350B's PSRAM select) is untouched.
- Hi-Z is not isolation. mozzwald's bring-up notes warn that USB-powered GPIO
  can back-power an unpowered peer; decide the power-up order of the Visor and
  the USB side on purpose.
- Program the first image with the module detached from the Visor, using
  BOOTSEL, as his bring-up procedure does. When several RP2350 boards are
  attached, pass `picotool --ser` so the right one is flashed.
- The Prism shares OE with its own flash, so a seated module on a running
  Visor sees OE toggling constantly. A slot that shows nothing is dark or
  badly seated (`watch`).
- The USB device uses the Pico SDK's Raspberry Pi vendor and product IDs so
  that `picotool -f` can reboot the board through the reset interface. A
  product ID of its own should be requested before the module is distributed.

## Pins

The carrier connects the Springboard bus to the Core2350B like this. GPIO
numbers are what the firmware uses; pads are the Core2350B's.

| GPIO | Signal | Pad | GPIO | Signal | Pad |
| --- | --- | --- | --- | --- | --- |
| 0 | /IRQ (not driven) | A2 | 24 | D4 | J6 |
| 1 | A1 | B2 | 25 | D5 | K6 |
| 2 | A2 | B1 | 26 | D6 | J7 |
| 3 | A3 | C2 | 27 | D7 | K7 |
| 4 | A4 | C1 | 28 | D8 | J8 |
| 5 | A5 | D2 | 29 | D9 | K8 |
| 6 | A6 | D1 | 30 | D10 | K9 |
| 7 | A7 | E2 | 31 | D11 | J9 |
| 8 | A8 | E1 | 32 | D12 | J10 |
| 9 | A9 | F2 | 33 | D13 | H9 |
| 10 | A10 | F1 | 34 | D14 | H10 |
| 11 | A11 | G2 | 35 | D15 | G9 |
| 12 | A12 | G1 | 36 | A16 | G10 |
| 13 | A13 | H2 | 37 | A17 | F9 |
| 14 | A14 | H1 | 38 | A18 | F10 |
| 15 | A15 | J1 | 39 | LED, test point TP3 | E9 |
| 16 | /CS0 | J2 | 40 | A19 | E10 |
| 17 | /CS1 | K2 | 41 | A20 | D9 |
| 18 | /OE | J3 | 42 | A21 | D10 |
| 19 | /WE | K3 | 43 | A22 | C9 |
| 20 | D0 | J4 | 44 | A23 | C10 |
| 21 | D1 | K4 | 45 | /RESET | B10 |
| 22 | D2 | J5 | 46 | LOWBAT | B9 |
| 23 | D3 | K5 | 47 | unused, test point TP4 | A9 |

The bus is 16 bits wide, so A1 is the lowest address line. PIO1 and PIO2 run
with GPIO base 16, which puts the selects, D0-D15 and A16-A23 in
one 32-bit snapshot; `src/bus.h` defines how a CS1 write is decoded from it.

## How it works

- **CS0 reads**: the Palm's reads of the ROM. PIO1 holds the data pads and
  Core1 looks each address up in a 64 KiB table in SRAM, in a persistent loop
  that lives in SCRATCH_X so that Core0's SRAM traffic cannot stall its
  instruction fetches.
- **CS1 reads**: the same engine answers them from a second table, the
  registers and the reply. Core0 writes to that table sparingly (a clock
  register once a millisecond, the rest when something happens), because its
  SRAM traffic delays Core1's replies.
- **CS1 writes**: an input-only PIO2 state machine snapshots GPIO16-47 shortly
  after a write starts, DMA streams the snapshots into a ring and Core0 turns
  them into register writes (`src/link.c`). Nothing in this path drives the
  bus.
- **To NIO**: Core0 sends each request to NIO over USB CDC port 0 with the
  packet-link controller in `src/packet_link.c`, which has no SDK calls so
  the host tests build it too, and publishes the answer.
- **Console**: CDC port 1 is a text console (below). A third USB interface is
  Raspberry Pi's reset interface, so `picotool load -f` needs no BOOTSEL
  button.

The firmware is copied to RAM at boot and runs at 150 MHz.

## Reproducible setup

Run these scripts from this directory, or invoke them by path from anywhere.
They locate the project and source the workspace environment automatically when
available. You do not need to export `NIO_WORKSPACE` or assemble a CMake recipe.

| Command | Purpose |
| --- | --- |
| `./scripts/setup.sh` | Prepare all pinned sources and check build prerequisites. |
| `./scripts/setup.sh --install-toolchain` | Also install the verified local Arm compiler if needed. |
| `./scripts/setup.sh --repair` | Preserve and replace altered local dependency checkouts. |
| `./scripts/setup.sh --host-only` | Prepare native tests only; no SDK or Arm compiler. |
| `./scripts/create-deps.sh` | Fetch/validate source dependencies only, without compiler setup. |
| `./scripts/create-deps.sh --check` | Check dependencies without downloading or modifying them. |
| `./scripts/test.sh` | Configure, build and test native Debug (ASan and UBSan) and Release. |
| `./scripts/build.sh firmware` | Build the firmware with the placeholder ROM. |
| `./scripts/build.sh firmware --rom FILE` | Build it serving the Springboard ROM `FILE`. |

Each script has `--help`. Setup is idempotent: clean pinned dependencies are
reused. Build scripts perform their required source setup too. No command here
loads a board or touches a serial port.

Prerequisites: Git, Python 3, CMake >=3.21, Ninja and a native C compiler.
Firmware needs the Arm GNU 14.2.Rel1 embedded toolchain (`arm-none-eabi-gcc`
and `g++`). `--install-toolchain` verifies and installs the pinned Linux x86_64
archive only; on any other host install the toolchain yourself and set
`PICO_TOOLCHAIN_PATH` to its `bin` directory (the build was verified on macOS
arm64 with `arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi`). Setup
reports missing tools; it does not install system packages or run sudo.

`./scripts/setup.sh --repair` restores dependencies that were formatted or
edited by accident; it preserves them under `.deps-backups/` first and leaves
clean dependencies alone. The bridge's `.clang-format-ignore` excludes
dependencies, backups and build products from file-based clang-format runs;
editor integrations may need matching exclusions. `PICO_SDK_PATH` selects an
existing SDK, which is validated against the pin but never repaired.

Source pins in `dependencies.json` are the same as the Zorro bridge's:

| Dependency | Release | Commit |
| --- | --- | --- |
| apio | v0.3.0 | `1023d866849694417ced497e17e98a2cd2bd026e` |
| epio | v0.2.1 | `bf49b19a534befb69bd518ee463365a1a5d73a65` |
| Pico SDK | 2.3.1 | `079c6f39023649b154152db30f1d781e884879bc` |
| picotool (UF2 host tool) | 2.3.1 | `2041936441b48a3cc53ae3da9e805229fe8f4e18` |

`scripts/bootstrap.py` is the low-level fetch and validation used by CMake;
sources land in the ignored `.deps/`. Changing a version is a separate
deliberate edit to `dependencies.json`. First-party PIO is written with apio
macros and the SDK's `pio_encode_*`; there are no `.pio` files and no pioasm,
and every configure and build checks that (`scripts/check_pio_policy.py`).

## The ROM

The ROM served on CS0 is a Springboard ROM image of 512 to 65,536 bytes, an
even number, holding the Handspring card header and the Palm apps. It is built
outside this tree; see [dillera/springboard-nio](https://github.com/dillera/springboard-nio)
and mozzwald's [fujinet-palm](https://github.com/mozzwald/fujinet-palm).
`scripts/rom2c.py` turns it into the C array the firmware embeds:

```sh
./scripts/build.sh firmware --rom /absolute/path/to/sbnio.rom
```

Without a ROM the build prints a notice and embeds a 512-byte erased image
(all 0xFF). It has no card header, so a Visor has nothing to list; it exists
so the firmware, the link and the console can be built and exercised without a
Palm toolchain. It has not been tried in a Visor.

Outputs: `build/firmware/sbnio.elf`, `sbnio.uf2` and `sbnio.bin`. The console
banner shows the embedded ROM's size and CRC-32.

## Flashing

No command above flashes. With the module detached (see Safety), hold BOOTSEL
while plugging the board in and copy `sbnio.uf2` to the drive, or, when
the board already runs this firmware, use its reset interface:

```sh
picotool load -x build/firmware/sbnio.uf2 -f --ser <usb-serial-number>
```

## Console

`./scripts/sbcon.py` talks to the console port; it finds the port by its USB
name (macOS, and by `/dev/serial/by-id` on Linux, where it has not been run),
or from `SBNIO_CONSOLE`, and `SBNIO_SERIAL` picks one board of several.

```sh
./scripts/sbcon.py                      # watch the console
./scripts/sbcon.py status regs          # run commands, print the output
./scripts/sbcon.py "send 450106004c00"  # Clock.GetTime straight to NIO
./scripts/sbcon.py --port "NIO link"    # the device NIO should open
```

| Command | What it does |
| --- | --- |
| `status` | uptime and reset reason, bus and CS0/CS1 cycle counts, write counts, WE low times, the NIO link state and its counters |
| `pins`, `watch`, `scan [ms]` | the live bus levels; level changes on the control lines for 200 ms; level changes on every GPIO |
| `float` | with the engine stopped, pull a few bus pins down with their input buffers off (RP2350-E9) to see which the Visor drives |
| `regs` | the CS1 register table |
| `send <hex>` | send a raw FujiBus packet to NIO as sequence 0, without a Palm |
| `trace on\|off` | log every CS1 write |
| `loopback on\|off` | answer requests with themselves instead of asking NIO |
| `settle [0-31]` | PIO cycles from seeing a write to sampling it |
| `widths` | measured WE low times, then reset them |
| `stop`, `arm` | stop the bus engine and release the pads; start it again |
| `bootsel` | reboot into BOOTSEL |

## Tests

`./scripts/test.sh` builds and runs these natively with no hardware, Debug
(with AddressSanitizer and UBSan) and Release, and every test is active under
`NDEBUG`:

| Test | Checks |
| --- | --- |
| `packet_link` | the packet-link controller against a scripted peer, including its record bytes |
| `sbnio_abi` | the `SBN_CHECK_STEP` check word, and that registers sit below `SBN_R_RESP`, the reply fits the 64 KiB window and the write register offsets are as ABI 4 says |
| `bus_write` | `BUS_WRITE_REG` and `BUS_WRITE_DATA` against snapshots built pin by pin |
| `pio_rearm` | the read engine's PIO program in epio against a model of Core1: paired CS0 and CS1 reads, OE and select overlap, cancellation, a stalled CPU, RESET, writes ignored |
| `tooling`, `setup` | dependency bootstrap, the build scripts, `rom2c.py` and the PIO policy |
| `pio_policy` | no `.pio` sources or pioasm rules |

Epio does not model the input synchronizer delay, so `pio_rearm` shows
instruction behaviour in the emulator, not bus timing, which was measured on
hardware (Status). No test runs Core1's assembly loop; `pio_rearm` models it.
