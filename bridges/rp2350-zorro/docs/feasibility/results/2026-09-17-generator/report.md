# RP2040 W0 generator bring-up — 2026-09-17

Scope: independent generator validation only. The RP2350 firmware was not changed
or tested. These observations do not establish RP2350 capture or Zorro feasibility.

## Equipment and fixture

- TZT Pico-style RP2040 B2, verified flash ID `754765170F445253`, 16 MB reported
  by the bootloader. Generator runs from SRAM; existing Debug Probe flash was
  not overwritten. RAM USB descriptor serial is `EEEEEEEEEEEEEEEE`.
- Core2350B connected in W0, USB serial `DCD9EB3F6D168102`; no console operations,
  new firmware or DUT-result claims in this experiment.
- Independent USB `fx2lafw` analyzer, VID/PID `0925:3881`, enumerated as Saleae
  Logic. This USB identity does not authenticate its manufacturer or calibration.
- RP2040 GP2–5 -> DUT GP2–5 -> physical CH1–4 (sigrok D0–3).
  RP2040 GP6 -> DUT GP1 -> physical CH8 (sigrok D7). Common GND; 3.3 V fixture.
  CLK and CH5–7 unused. User wired the fixture; no photograph, lead-length
  measurement or independent electrical continuity record is attached.
- Capture: 1 MHz, 1,000,000 samples per run, eight-bit raw storage with D0–3/D7
  selected. Scope and multimeter were available but not used for these results.

## First observed firmware

Firmware baseline before this change:
`eceaa5447d922c65ccb258b72514206e9f1fe135`, with generator implementation dirty.
[Metadata](metadata.json) records loaded ELF/UF2 SHA-256 identifiers. Dependencies:
SDK 2.3.1, apio v0.3.0 and epio v0.2.1 at the bridge's locked revisions; ARM GNU
14.2.Rel1 toolchain. The image was inspected as SRAM address `0x20000000`, then
loaded and read-back verified using pinned USB-enabled picotool 2.3.1:

```sh
build/toolchains/picotool-usb/picotool load -v -x \
  repos/fujinet-nio/bridges/rp2350-zorro/build/stimulus-rp2040/feasibility_stimulus.elf \
  --bus 7 --address 16
```

This was the actual identified bootloader address at that moment, not a reusable
load command. The current generator guide requires fresh target identification.

## Results

| Run | Expected | Observed | Verdict |
| --- | --- | --- | --- |
| [001](w0-burst-001.sr) | One explicit `run`; 16 ascending samples | 16 falling edges, values 0–15, 100 us low intervals | Pass for this generator waveform |
| [002](w0-burst-002.sr) | Fresh USB console, second explicit run | Same 16 values and timings | Pass for normal rearm/reconnect |

Both runs measured 100 us setup for values 1–15 and 100 us hold for values 0–14.
All consecutive falling edges were 300 us apart, hence intervening high intervals
were 200 us. Values remained correct throughout each asserted interval. Console
reports agreed with 16 completed samples. No extra assertion was detected in
these one-second captures.

The independent checker accepts ±2 us for interval comparisons; the reported
values above are the actual sample-count measurements, not that tolerance.
Sampling resolution is 1 us. Analyzer clock accuracy and analog thresholds are
uncalibrated, so these are instrument-reported intervals, not traceable absolute
timing bounds or Zorro margins. The first zero-valued data update can be invisible
against an already-zero idle level; the last hold includes software release and
is excluded from the 100 us hold assertion. Neither is silently treated as measured.

The first two captures predate USB receive-queue cleanup/review changes. Their
loaded hashes remain in metadata; final-image results must be recorded separately.
The APIO waveform itself is unchanged, but this is not evidence for revised USB
session behavior.

## Final reviewed image

Executable source revision: `bbf69259883d9bc489642aea2778f623263e7c3a`.
[Final metadata](final-metadata.json) records artifact/capture hashes. ELF SHA-256:
`3ce6a0f7f5962e0fad13b73bffd9b8b43bb4611bd841de1d656d9c5c2c6fb1a4`.
The user loaded this reviewed RAM image with picotool and restored serial access
following re-enumeration. All observations below use this image.

| Run | Expected | Observed | Verdict |
| --- | --- | --- | --- |
| [Final 001](w0-final-001.sr) | 16 ascending values, nominal timing | 0–15, exactly 16 assertions; 100 us low/setup/hold and 300 us assertion spacing | Pass |
| [Session/invalid commands](w0-session.sr) | Partial `ru`, DTR down/up, then `n` must not run; binary and oversized lines rejected | Unknown-command plus two invalid-command errors; /AS high throughout one-second capture | Pass for tested transitions |
| [Active stop](w0-stop.sr) | Script sends `stop` approximately 1 ms after `run`; truncate burst | Four assertions carrying 0–3; `stopped generated=unknown`; /AS high for remainder after the final edge plus 1 ms | Pass for functional stop |
| [Final 002 after stop](w0-final-002.sr) | Fresh run restarts at zero and completes | 0–15, exactly 16 assertions and same timing as Final 001 | Pass for physical rearm |

These add 32 completed assertions and four before stop. The script's sleep is
not a synchronized command marker: no numerical stop-latency bound is claimed.
No trace here demonstrates electrical high impedance. Session tests exercise a
30 ms DTR-down interval; native queue/epoch tests additionally cover rapid
transitions, but those narrower races have not been physically timed.

One attempted final capture failed because PulseView held the analyzer USB
interface. Its [console transcript](failed-analyzer-busy.console.txt) shows a
completed firmware burst, but there is no corresponding valid analyzer capture;
it is excluded from measured counts above. PulseView was closed and acquisition
was repeated successfully. This is an acquisition failure, not a DUT timing result.

## Reproduce the trace analysis

From this evidence directory:

```sh
python3 check_trace.py w0-burst-001.sr
python3 check_trace.py w0-burst-002.sr
python3 check_trace.py w0-final-001.sr
python3 check_trace.py w0-final-002.sr
pulseview w0-final-001.sr
```

The checker reads the raw sigrok samples, counts /AS falling/rising edges and
checks independent literal values and timing intervals. The `.console.txt` and
`.measurements.json` files retain USB replies (CRLF normalized to LF) and per-edge results. Capture was
armed first, then a stdlib Python serial harness waited 350 ms and sent `run`;
USB only started the finite PIO program and did not pace its edges. The durable
[generator guide](../../../rp2040-generator.md) describes manual reproduction.

## Limits and remaining work

The accepted captures contain four complete bursts (64 assertions) plus four
before stop; there is no stress/phase sweep.
GPIO high impedance, electrical edge quality, actual stop latency, loss under
pressure and exhaustive disconnect/reconnect races are not established by these
captures. Functional stop and the specified reconnect case passed. No oscillator calibration, scope trace, bus-requirement margin or
real-bus result exists here. Story 2.2 remains in progress; the next physical
stage is the RP2350 observer after the generator is accepted.
