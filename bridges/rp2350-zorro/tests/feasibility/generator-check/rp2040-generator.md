# RP2040 W0 stimulus generator

This is the historical version of the original W0 stimulus generator documentation.
This is superceded by the README.md in this folder.

---

**Repeatable entry point:** use the [generator-check starter and source map](README.md).
Its staged runner replaces the ad hoc load/capture procedure from initial bring-up.

This isolated lab target emits one finite ascending four-bit burst after an
explicit USB `run`. It implements only the generator portion of E0–E3 in the
[Story 2.2 experiment plan](story-2-2-experiment-plan.md). It does not implement
the RP2350 observer, automated two-board runner, Zorro protocol, or mailbox ABI.

## Build and software checks

From the bridge directory, use `./scripts/setup.sh` once, or
`./scripts/setup.sh --repair` if vendored dependencies were accidentally edited.
See the [setup command table](../README.md#reproducible-setup) for toolchain options.

`./tests/feasibility/generator-check/run.sh build` performs the native Debug and
Release tests, RAM firmware build and USB-enabled loader build. It never loads or
starts a device. The workspace command `scripts/build.sh rp2040-stimulus` remains
available for the firmware-only build.

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
historical bench had flash identity `754765170F445253` and previously ran CMSIS-DAP Debug
Probe firmware. BOOTSEL inspection identified RP2040 B2 and reported 16 MB
of external flash; the generator deliberately does not depend on that flash.
The RP2350 DUT identity is `DCD9EB3F6D168102`; do not load this
image there. Other users enroll their own generator with `configure`, as described
in [bench setup](bench-setup.md). The starter discovers USB addresses afresh during BOOTSEL; it
does not reuse a bus address from an earlier run.

The generator starter builds its USB-enabled picotool from the pinned dependency.
Use its `load` stage: it gives BOOTSEL instructions, verifies the flash identity,
loads RAM and follows re-enumeration to the correct serial port. The separate
`run` stage asks before generating output and saves analyzer/console evidence.
Normal-user USB access must be configured as described in the
[one-time access setup](../tests/feasibility/README.md#one-time-linux-access-setup).
No changing bus addresses or per-device sudo commands belong in the normal flow.

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
is not unique if another RAM image is connected.

The starter owns the console and DTR session during a run. Its retained
`console.log` shows the commands and responses described below; do not open a
second serial application concurrently.

The baud setting is conventional USB CDC configuration; PIO sets signal timing.
The USB protocol accepts `help`, `status`, `run`, or `stop`. `run` has no parameters
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
only one application can claim its USB interface. The starter's `run` stage arms
a 0.25-second acquisition before sending `run` to the generator, and saves
`capture.sr` for PulseView. The duration is manifest controlled and may be
overridden for one run with `--acquisition-seconds 1`. Use `analyse --capture`
to check a saved file again. Ctrl-C cancels acquisition and attempts a generator
stop; failed runs retain their logs and verdict.

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

The starter uses an untriggered window to include the setup interval. Missing
pulses are not a passing measurement. Data zero may match the idle
level, so the first data-update instant is not always observable externally.
Later value transitions provide the measurable setup/hold oracle.

To exercise abort handling, a host script must send `run` followed immediately
by `stop`, or drop DTR, while acquisition is running. Retain both the console
result and trace: if completion wins the USB scheduling race, that run does not
verify active abort. The very short burst makes these races expected. Native
control tests cover the deterministic boundary; bounded physical stop latency,
release ordering and electrical high impedance remain separate unverified
measurements until a trace/appropriate bias fixture establishes them.
