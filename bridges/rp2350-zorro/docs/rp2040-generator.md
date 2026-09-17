# RP2040 W0 stimulus generator

This isolated lab target emits one finite ascending four-bit burst after an
explicit USB `run`. It implements only the generator portion of E0–E3 in the
[Story 2.2 experiment plan](story-2-2-experiment-plan.md). It does not implement
the RP2350 observer, automated two-board runner, Zorro protocol, or mailbox ABI.

## Build and software checks

From the workspace, `scripts/build.sh rp2040-stimulus` builds only; it never
loads or starts a device. `scripts/build.sh --explain rp2040-stimulus` lists the
steps. For the isolated project:

```sh
source "$NIO_WORKSPACE/scripts/env.sh"
cd "$NIO_WORKSPACE/repos/fujinet-nio/bridges/rp2350-zorro"
export PICO_TOOLCHAIN_PATH="$NIO_WORKSPACE/build/toolchains/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin"
python3 scripts/bootstrap.py --mode host
cmake --preset host
cmake --build --preset host
ctest --preset host
cmake --preset host-release
cmake --build --preset host-release
ctest --preset host-release
python3 scripts/bootstrap.py --mode stimulus
cmake --preset stimulus-rp2040
cmake --build --preset stimulus-rp2040
python3 scripts/check_pio_policy.py
```

Outputs: `build/stimulus-rp2040/feasibility_stimulus.elf` and
`build/stimulus-rp2040/feasibility_stimulus.uf2`. The preset selects the SDK
`pico` RP2040 configuration for clocks/USB, but forces `no_flash`: executable
code and initialized data load into SRAM, without a flash boot stage or flash
part assumption. Power cycling restores whatever firmware was already in flash.
The ELF/UF2 is a RAM image, not a persistent installation.

The same APIO-authored seven instruction words and register configuration
are used by native epio tests and the SDK PIO loader. No APIO RP2350 MMIO initialization runs on RP2040.
Existing dependency pins, validation guards and RP2350 presets remain in use.

## Identify and load only the generator

Identify the board before loading anything. The W0 generator observed on this
bench has flash identity `754765170F445253` and previously ran CMSIS-DAP Debug
Probe firmware. BOOTSEL inspection identified RP2040 B2 and reported 16 MB
of external flash; the generator deliberately does not depend on that flash.
The RP2350 DUT identity is `DCD9EB3F6D168102`; do not load this
image there. USB addresses change when entering BOOTSEL. Read fresh `lsusb`
output and correlate physical unplug/replug and `picotool info -a`.

The picotool packaged by the bridge build deliberately has no USB support.
For loading, use a USB-enabled picotool built from the same pinned source
(`.deps/picotool`) with libusb development files installed:

```sh
cmake -S .deps/picotool -B "$NIO_WORKSPACE/build/toolchains/picotool-usb" \
  -DPICO_SDK_PATH="$PWD/.deps/pico-sdk" -DPICOTOOL_NO_LIBUSB=OFF
cmake --build "$NIO_WORKSPACE/build/toolchains/picotool-usb" -j2
export PICOTOOL="$NIO_WORKSPACE/build/toolchains/picotool-usb/picotool"
lsusb
# Set these from the freshly identified RP2040 BOOTSEL entry, not an old address.
export GEN_BUS=REPLACE_WITH_BUS GEN_ADDRESS=REPLACE_WITH_ADDRESS
"$PICOTOOL" info -a --bus "$GEN_BUS" --address "$GEN_ADDRESS"
"$PICOTOOL" load -v -x build/stimulus-rp2040/feasibility_stimulus.elf \
  --bus "$GEN_BUS" --address "$GEN_ADDRESS"
```

Enter BOOTSEL by holding the board's BOOT button while plugging in USB (or
holding BOOT while pressing/releasing reset), then release BOOT. Loading needs
access to the selected `/dev/bus/usb/BBB/DDD` node. USB serial access after the
load separately needs access to its `/dev/ttyACM*` node. A permission error is a
host access issue; do not substitute a different board. The load command starts
the idle executable; it does not start the waveform.

## Wiring and USB control

| Generator | Signal | Analyzer physical input | sigrok channel |
| --- | --- | --- | --- |
| GP2 | D0 (least significant) | CH1 | D0 |
| GP3 | D1 | CH2 | D1 |
| GP4 | D2 | CH3 | D2 |
| GP5 | D3 | CH4 | D3 |
| GP6 | /AS | CH8 | D7 |
| GND | common ground | GND | — |

All signals are 3.3 V bench signals. Pins GP2–6 are inputs at boot and while idle.
GP6 has a weak internal pull-up to keep the released strobe high; data inputs
have no internal pulls. Released data levels are not part of the waveform oracle.

The RAM-only SDK USB identity currently uses placeholder serial
`EEEEEEEEEEEEEEEE` (USB VID:PID `2e8a:000a`), not the original flash identity.
Correlate the USB physical port and BOOTSEL-to-CDC transition; the placeholder
is not unique if another RAM image is connected. Find the newly enumerated
generator port, then open it with DTR asserted:

```sh
ls -l /dev/serial/by-id/
export GEN_PORT=/dev/serial/by-id/REPLACE_WITH_IDENTIFIED_GENERATOR
uv run --with pyserial==3.5 python -m serial.tools.miniterm "$GEN_PORT" 115200 --raw
```

The baud setting is conventional USB CDC configuration; PIO sets signal timing.
Enter `help`, `status`, `run`, or `stop`, followed by Enter. `run` has no parameters
and emits exactly 16 samples. A second `run` while active returns `error busy`.
Unknown commands, binary control bytes and lines exceeding 31 characters are
rejected; an overflowing line is discarded through its newline. CR, LF and CRLF
are accepted. Boot/connection reports `idle generated=0`.

Completion reports `complete generated=16 nominal_hz=100000`. Stop, detected
USB/DTR disconnection and a 100 ms software deadline disable the state machine,
deassert /AS high for 10 us, then release all five pins. Abort counts are reported
as `unknown`, never as 16. `status` retains the last result after reconnection.
USB receive bytes queued before a detected disconnect/reconnect are discarded;
wait for the idle/status greeting before sending a new command. The control loop checks safety every iteration and consumes at most one input
byte per iteration. USB replies never wait for a reader: if the USB transmit
buffer is full a reply may be dropped; query `status` again. The normal burst
ends in a parking instruction even if the host never reads its result.

The polling loop adds USB/SDK scheduling latency to stop/disconnect response;
its numerical worst case has not been measured on hardware. The 100 ms deadline
is a software backstop, not a measured hard real-time response guarantee. A
complete burst finishes nominally in 4.82 ms, so manually typed stop will usually
arrive after completion. Firmware hangs beyond the control loop are not covered
by this deadline.

## Independent analyzer oracle

Close or disconnect the live analyzer in PulseView before using sigrok-cli;
only one application can claim its USB interface. In another terminal, arm the
analyzer **before** entering `run` in the console:

```sh
sigrok-cli --scan
sigrok-cli --driver fx2lafw --config samplerate=1m \
  --channels D0,D1,D2,D3,D7 --triggers D7=f --wait-trigger \
  --samples 10000 --output-file /tmp/rp2040-w0.sr
sigrok-cli --input-file /tmp/rp2040-w0.sr --output-format csv \
  --output-file /tmp/rp2040-w0.csv
pulseview /tmp/rp2040-w0.sr
```

The trigger waits for the first /AS falling edge. It will wait indefinitely if
no run occurs; Ctrl-C cancels the host capture without starting any output.
At each /AS falling edge, interpret CH4..CH1 as a four-bit binary number.
The independent expected sequence is exactly `0,1,2,...,15`, with 16 falling
edges. Relative to the first falling edge, subsequent falling edges occur at
nominal multiples of 300 us. Each low interval is 100 us; each intervening high
interval is 200 us. Data changes 100 us before each falling edge, remains stable
through the 100 us low interval, and stays stable for 100 us after rising.
The falling-edge-triggered capture omits setup before the first edge; use an
untriggered/pre-trigger capture to measure that first setup separately.

At the 100 kHz PIO clock, emulator cycle 1 sets X, cycle 2 updates the first
data value, cycles 12/22 are the first falling/rising edges, and cycle 482 sets
the completion IRQ. Each next sample adds 30 cycles. Native tests check every
cycle including parking beyond completion and remain active in Release.

A 1 MHz analyzer provides ten samples per PIO cycle; reported edge times still
have sample quantization and physical clock error. epio tests prove the
instruction/state behavior only. USB response time, actual GPIO timing, signal
integrity, and stop/release ordering need physical evidence. The RP2350 is already connected as the W0 fixture, but this slice neither
changes nor validates its firmware. Follow the staged experiment plan before
claiming Story 2.2 accepted.


## Recorded bench result and an untriggered capture

The [generator bring-up record](feasibility/results/2026-09-17-generator/report.md)
contains raw PulseView sessions, console logs and a reproducible independent
trace checker. It separates the tested firmware hashes and the later USB
hardening changes; consult its final-image status before assuming equivalence.

For a capture that includes the first setup interval, start this in a second
terminal, then promptly issue `run` in the already-connected generator console:

```sh
sigrok-cli --driver fx2lafw --config samplerate=1m \
  --channels D0,D1,D2,D3,D7 --samples 5000000 \
  --output-file /tmp/rp2040-w0-full.sr
```

This has a five-second capture window. If the run falls outside it, repeat;
missing pulses are not a passing measurement. Data zero may match the idle
level, so the first data-update instant is not always observable externally.
Later value transitions provide the measurable setup/hold oracle.

To exercise abort handling, a host script must send `run` followed immediately
by `stop`, or drop DTR, while acquisition is running. Retain both the console
result and trace: if completion wins the USB scheduling race, that run does not
verify active abort. The very short burst makes these races expected. Native
control tests cover the deterministic boundary; bounded physical stop latency,
release ordering and electrical high impedance remain separate unverified
measurements until a trace/appropriate bias fixture establishes them.
