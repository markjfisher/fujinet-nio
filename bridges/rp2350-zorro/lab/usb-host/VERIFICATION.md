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

## Idle USB stalls the first core tick

Mark's next capture confirmed the countdown and USB logs, LittleFS mounted,
transport setup completed, and startup reached `starting main loop`. No prompt
or phase-1 completion followed. A reset reproduced that sequence. This proves
USB output and startup through the loop entry, but not loop progress or USB RX.

The USB byte channel returned driver readiness from `available()` even with an
empty RX FIFO. `PacketLink::receive()` then repeatedly called `fill()` and
retried because `available()` remained true, never returning to console polling.
Changed the adapter to check the selected CDC interface's actual buffered input.
No packet protocol or settings changed.

Verification (workspace environment sourced, commands in firmware root):

- `python3 tests/test_esp32_usb_cdc_channel.py`: before the fix, idle receive
  timed out for both port selections; after the fix, passed. Compiles the actual
  USB adapter and PacketLink against mocked USB/RTOS calls. Covers idle input,
  isolation from the other CDC port, partial reads, partial records and failed
  initialization. It does not emulate USB enumeration or electrical behaviour.
- `cmake --preset fujibus-pty-debug` and
  `ctest --test-dir build/fujibus-pty-debug -R '^esp32-usb-cdc-channel$' --output-on-failure`:
  passed (1 test).
- `./build.sh -b`: bench N16R8 firmware passed.
- `git diff --check`: passed.

Next physical check: upload this build, reconnect the console after reset, and
confirm the prompt, `help`, `core.info`, and phase-1 service startup. Repeat
`core.info` to verify advancing ticks. The five-second LED flash remains
unexplained; no physical console recovery is claimed yet.

## ESP standalone console recovery confirmed

On 2026-11-06 Mark confirmed recovery after flashing the USB readability fix
(`27d83ce4`). The supplied capture shows:

- 13:57:50: startup reaches the main loop and prints the interactive prompt.
- 13:57:52: ClockDevice (69) and NetworkDevice (253) are registered; phase 1
  completes in 1 ms.
- `help` works (reported by Mark).
- `cd flash:/` and `ls` work; the listing shows a 1020-byte `fujinet.yaml`.
- `hexdump fujinet.yaml` returns readable contents from internal flash.

This confirms console RX/TX, main-loop progress, phase-1 startup and internal
flash reads on the physical ESP. The SD/config-primary and absent boot-image
warnings do not prevent these operations. No SD card or filesystem upload was
required. The temporary countdown remains enabled for the next bench run.

RP-to-ESP enumeration, PacketLink synchronization and Clock.GetTime remain
unverified. Next checkpoint is Run-mode wiring/power and capture of the RP UART
console through ESP-Prog. The five-second LED activity is still unexplained;
this report does not establish whether it persisted after the fix.

Documentation-only update: `git diff --check` passed; evidence is the user's
physical console capture, not an additional firmware build.

## First physical RP-to-ESP clock exchange

On 2026-11-09 Mark reported that RP GP0 and GP1 had been swapped during the
earlier UART troubleshooting; after correcting the wiring, picocom showed a
legible RP startup log with both boards powered from the shared 5 V bench rail:

```text
USB host probe: UART0 GP0 TX / GP1 RX, 115200 8N1; NO AMIGA
CDC mount index=0 address=1 interface=0
CDC mount index=1 address=1 interface=2
completion outcome=0 bytes=19 elapsed_ms=22 crc32=dc193838
clock valid status=0 unix_seconds=26 (nonzero status is a service error)
```

This establishes physical enumeration of the expected packet and console CDC
interfaces, PacketLink synchronization, and a successful 19-byte Clock.GetTime
response through the RP-to-ESP link. The 22 ms is a single observed completion,
not a latency benchmark. The returned value 26 is not a credible current Unix
time; a successful clock response does not establish a synchronized clock.
Mark also captured the startup with the logic analyzer; its file location and
capture details were not supplied for this run.

Next checks: confirm the RP console can receive `status` and `clock`, save their
output, then exercise larger known data and link recovery. Shared-connector
flashing, maximum-size transfers, fault handling and repeatable performance
remain unverified. `git diff --check` is the verification for this documentation
update; no firmware was changed or rebuilt.

Mark subsequently confirmed that the ESP-Prog TX wire had been reconnected and
the RP console accepted commands (without echo). Its `status` and a new
`clock` returned:

```text
state=ready data=0 logs=1 generation=1 capacity=4096 tx=0 fault=0 requests=1 answers=1 resends=0 timeouts=0 corrupt=0 log_bytes=265
completion outcome=0 bytes=19 elapsed_ms=4 crc32=eb2edab5
clock valid status=0 unix_seconds=243 (nonzero status is a service error)
```

This confirms RP UART input and output, a second successful clock request, and
the reported ready state and counters after the first request. The 4 ms and
earlier 22 ms observations are individual samples, not a performance study.
The next unchecked link test is known data across multiple USB transfers up to
capacity, including explicit capacity-plus-one rejection; fault recovery and
shared-connector flashing also remain open. Documentation check: `git diff
--check`.

## Known-data bench probe prepared

The RP probe now provides `bulk`, using existing FileDevice v1 on ESP flash.
It verifies a unique scratch path does not exist, writes a deterministic
pattern in chunks and compares every byte of the read-back. At a negotiated
capacity of 4096, one write request and the read response are 4096 raw bytes;
the file contains 4079 pattern bytes. The file remains on ESP flash.
`oversize` verifies that the local controller refuses capacity + 1 without
sending; this does not establish how the ESP handles a malformed oversized
record. No ESP protocol or firmware change is needed.

From `bridges/rp2350-zorro`, with the workspace environment sourced:

- `cmake --preset host`, `cmake --build --preset host --target test_usb_probe test_usb_packet_link`,
  `ctest --preset host -R '^usb_' --output-on-failure`: both tests passed.
  The probe test covers the generated 4096-byte write, exact response, corrupt
  data with a valid checksum and existing-file protection; checksum fixtures
  were cross-checked with `py/fujinet_tools/fujibus.py`. Controller tests
  cover local capacity rejection without sending.
- `./scripts/build.sh usb-host-rp2350`: passed. UF2 SHA-256:
  `023518b5bae835b3dfc193da80c2e43701afd3db2f9100ff7dadd1601cd1a393`.
- `git diff --check`: passed.

Physical transfer, long-record handling, isolation and recovery remain pending.

## Full-size known-data exchange confirmed

On 2026-11-09 Mark flashed the known-data RP probe and reported this Run-mode
result (ESP firmware unchanged):

```text
state=ready data=0 logs=1 generation=1 capacity=4096 tx=0 fault=0 requests=1 answers=1 resends=0 timeouts=0 corrupt=0 log_bytes=314
completion outcome=1 bytes=0 elapsed_ms=0 crc32=00000000
oversize PASS (local capacity rejection; no request sent)
bulk starting: size=4079 capacity=4096 file=flash:/zorro-usb-bench-000000000408a8e3.bin (retained on flash)
bulk stage=stat request=52 data=0 offset=0
completion outcome=0 bytes=27 elapsed_ms=25 crc32=f4eeaac9
bulk stage=write request=4096 data=4038 offset=0
completion outcome=0 bytes=17 elapsed_ms=236 crc32=4f730e72
bulk stage=write request=99 data=41 offset=4038
completion outcome=0 bytes=17 elapsed_ms=78 crc32=a3b3c39b
bulk stage=read request=58 data=4079 offset=4079
completion outcome=0 bytes=4096 elapsed_ms=99 crc32=64e3233f
bulk PASS: 4079 data bytes compared; response=4096 capacity=4096 file=flash:/zorro-usb-bench-000000000408a8e3.bin
state=ready data=0 logs=1 generation=1 capacity=4096 tx=0 fault=0 requests=5 answers=5 resends=0 timeouts=0 corrupt=0 log_bytes=3836
```

This verifies a full-capacity request and response, multi-packet USB data in
both directions, byte-for-byte read-back, and local capacity-plus-one rejection.
It does not test peer rejection of an oversized wire record, failure recovery,
or establish a latency/throughput distribution; elapsed values are single
samples including ESP file operation and debug activity. The scratch file is
retained on internal flash. Documentation-only check: `git diff --check`.

## Idle ESP reset observation

On 2026-11-09 Mark reported removing the temporary ESP countdown and pressing
ESP reset while the link was idle. The RP reported both CDC interfaces
unmounted and mounted again. Its automatic Clock.GetTime completed with 7 bytes
and CRC32 `2d67f8ad`, followed by `FAIL invalid Clock.GetTime response`.

Inference, not a raw capture: CRC32 matches the valid FujiBus bytes
`45 00 07 00 4e 01 01`, representing device 0x45, command 0 and status 1
(`DeviceNotFound`). The packet was not printed, so the CRC match does not prove
its contents. Source inspection shows an unknown device response leaves its
command at zero, and ClockDevice is registered during phase 1 after the main
loop has run for 100 ticks. The automatic request can therefore precede clock
registration; the probe rejects command 0 when expecting command 1.

Next verify `status` and a fresh manual `clock` after startup settles. A success
would show the reset link and clock service recovered; the initial response
must still be reported as a service/startup issue, not counted as an automatic
clock success. In-flight reset and other fault cases remain untested. This is a
documentation-only observation, checked with `git diff --check`.

After startup settled, Mark reported:

```text
state=ready data=0 logs=1 generation=2 capacity=4096 tx=0 fault=0 requests=6 answers=6 resends=0 timeouts=0 corrupt=0 log_bytes=4150
completion outcome=0 bytes=19 elapsed_ms=21 crc32=29b795de
clock valid status=0 unix_seconds=130 (nonzero status is a service error)
```

The link returned to ready in a fresh generation and a new manual clock
request succeeded. This supports the early-service timing explanation, though
the earlier response's bytes were not captured. Idle ESP reset recovery is
observed; the first automatic clock request still needs correct startup
handling, and in-flight reset/recovery remains untested. Documentation-only
check: `git diff --check`.

## In-flight reset probe prepared

The RP bench image provides `stream` for repeated independent read-only clock
requests and `stop` for manual termination. It prints the pending flag on USB
disconnect and stops rather than replaying a request. A pending unknown
completion keeps the fault latched across USB remount, requiring explicit
`resync` before a fresh request. Automatic startup clock requests are suppressed
after a stream disconnect. A reset that reports `pending=0` is not evidence of
an in-flight interruption.

From the bridge, with the workspace environment sourced,
`cmake --build --preset host --target test_usb_probe test_usb_packet_link`,
`ctest --preset host -R '^usb_' --output-on-failure`, and
`./scripts/build.sh usb-host-rp2350` passed. The existing native tests cover
the packet controller and probe helpers, not this new Pico USB callback flow;
physical stream/reset verification remains pending. UF2 SHA-256:
`bcf14df9c40887bf780d1a556238b4baf1a39437bb3925361b602c2d7b63b29e`.
Documentation and diff check: `git diff --check`.

## ESP reset during clock stream

On 2026-11-09 Mark ran the stream/reset probe on the physical fixture. One run
reported `pending=0` after 47 successful stream requests and returned to ready
in generation 2 with fault=0, requests=48, answers=48, resends=0, timeouts=0
and corrupt=0. This was another idle-reset observation.

After further attempts, the reset interrupted request 16:

```text
CDC unmount index=0
stream disconnect: pending=1 seq=16
completion outcome=2 bytes=0 elapsed_ms=7 crc32=00000000
stream STOP seq=16 outcome=2 valid=0 status=0
CDC unmount index=1
FAULT latched; uncertain request will not be replayed. status / resync
CDC mount index=0 address=1 interface=0
FAULT retained across reconnect; use resync before a NEW request
CDC mount index=1 address=1 interface=2
```

Outcome 2 is `PL_UNANSWERED`: the request may have run. This demonstrates
in-flight disconnect detection, stream stop and fault retention on reconnect.
The capture does not establish whether the ESP processed request 16. Explicit
resynchronization and a fresh successful request remain to be verified.
Documentation-only check: `git diff --check`.

Mark then verified recovery after the in-flight reset:

```text
status: state=off data=0 logs=1 generation=5 capacity=4096 tx=0 fault=1 requests=86 answers=85 resends=0 timeouts=0 corrupt=0 log_bytes=23233
resync: (no output)
status: state=ready data=0 logs=1 generation=6 capacity=4096 tx=0 fault=0 requests=86 answers=85 resends=0 timeouts=0 corrupt=0 log_bytes=23233
clock: completion outcome=0 bytes=19 elapsed_ms=4 crc32=1127a29c
clock valid status=0 unix_seconds=88 (nonzero status is a service error)
```

The request and answer counters were unchanged by `resync`; a fresh clock
request then succeeded in the new generation. This completes the observed
ESP-reset recovery case. It does not establish whether the interrupted request
ran, and does not cover RP reset, physical cable disconnect, stalled receiver,
partial record, or shared-connector flashing. Documentation-only check:
`git diff --check`.

## Idle RP reset

On 2026-11-09 Mark reset the RP while the link was idle and the ESP remained
powered. The RP restarted, mounted CDC interfaces 0 and 2, and its automatic
clock request completed successfully (19 bytes, 25 ms, status 0). `status`
reported `state=ready`, generation 1, capacity 4096, fault 0, one request and
answer, and zero resends, timeouts and corrupt records. A fresh manual clock
request also succeeded (19 bytes, 9 ms, status 0, unix_seconds=540).

This confirms the observed idle RP reset recovery. The generation and counters
restart with the RP, so generation 1 is not evidence of a globally unique
generation. An RP reset while a request is in flight remains untested and the
RP's volatile state cannot establish the interrupted request's outcome.
Documentation-only check: `git diff --check`.
