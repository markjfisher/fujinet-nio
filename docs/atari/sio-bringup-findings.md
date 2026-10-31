# Atari SIO bring-up: what the hardware measurements showed

Findings from running fujinet-nio on a FujiNet v1 board (FN32ROV-1.6, ESP32-S3) against an
Atari 800XL, October 2026. Use it to know which timings are healthy and which design rules
came from real measurements. Re-measure before relying on a number for a different board.

The native endpoint retains its most recently read response until the next
accepted W or reset. An exact retry of that R returns the same response and
padding, so an OS SIO retry after a lost data frame does not receive a NAK.
P still reports idle after the first R; a different R length is refused.

## How to measure

`sio.reset` before powering the Atari, boot it, run the boot disk's FTESTNIO to the end, then
`sio.status` (see [diagnostics](../diagnostics.md)). Cold-boot both together with no USB cable
(the FujiNet powered from the Atari) as well as with the Atari power-cycled alone: the boot
timing differs. Logs show `[N ms] starting main loop`, `[N ms] phase 1 done in M ms` and
`[N ms] wifi bring-up done in M ms`.

## Healthy numbers (D1 boot path, 19200 baud)

| Measure | Seen | Limit / meaning |
|---|---|---|
| Ingress ready | about 0.4-0.5 s after reset | before the Atari's first command |
| First real command | about 2.9-3.0 s after power-up, every run | set by the Atari, not the FujiNet |
| CMD release to first reply byte (`reply` max) | 9-10 ms quiet, 13 ms under a failing Wi-Fi connect before pinning | the Atari gives up after 16 ms |
| CMD release to start of processing (`react` max) | about 5 ms | |
| Handler | max 12-14 ms | slow work here delays the reply |
| Late ACKs, timeouts, UART errors | 0 | |

A 97-sector boot image is dominated by 256-byte sector transfers at 19200 baud (about 180 ms
each), not by FujiNet processing; single density or double density sectors are a bus-time
question, not a firmware one.

## What went wrong and why

1. **No ACKs at all.** The ESP-IDF call `gpio_set_direction()` on the TX pin re-routed it to a
   plain GPIO output and detached the UART. Never touch a UART-owned pin that way
   (`atari_sio_ingress.cpp`).
2. **The native 0x7f endpoint wedged after a timed-out call.** A native `W` runs its handler
   synchronously. The first Wi-Fi request on an unconfigured device initialised the radio
   inside that handler (a second or more), the Atari timed out (`$8A`), and the finished reply
   then sat in the `Ready` state, so every later `Q` or `W` was NAKed (`$8B`). Fixes:
   - Wi-Fi init and connect run in their own priority-1 task on core 0; the Wi-Fi service answers
     "not ready" until the link is published (`main_esp32.cpp`).
   - A reply left unread for 2 s is discarded by the next `Q` or `W` (`AtariSioNioFrontend`).
     `P` and `R` never discard it. This is covered by unit tests only; the original trigger is
     gone, so it cannot be provoked on hardware.
3. **The SIO task was starved by the radio.** With a failing Wi-Fi connect the worst reply was
   13.3 ms against the 16 ms deadline. Pinning the core/SIO task to core 1 (the Wi-Fi driver is
   pinned to core 0) brought it to 9.2 ms with the same bad SSID. The CMD interrupt and SIO
   UART are installed by that task, so they run on core 1 too.

## TX pacing

The SIO UART used the default `tx_chunk_gap_us` of 2000 (a 2 ms idle after every 16 bytes), meant for
Amiga RS-232 overruns. The SIO ingress now forces all TX pacing off, whatever `fujinet.yaml` says:
a POKEY needs none, and the gaps added about 30 ms to each 256-byte sector reply.

## Misreadings to avoid

- The "first CMD edge" in `sio.status` is often not a command. With Wi-Fi active a transient
  appears around 1.5-1.7 s, long before the Atari's first command at about 2.9 s. An early first
  edge with a much later first frame is not by itself evidence of missed commands; check `react`
  and `reply`.
- `aborts edge` counts extra CMD edges between polls. It is 0 with Wi-Fi off, about 6 during a
  failing connect and 100-150 with Wi-Fi connected and traffic flowing; it stays at 0 with
  Wi-Fi connected and the Atari idle. It tracks radio activity, so it looks like coupling onto
  the CMD input (GPIO39), not a firmware fault. Frames are still served (the Atari retries), so
  it costs a little time only. If it ever matters, look at the CMD input hardware (pull-up,
  filtering) before the firmware.
- The `poll gap` metric first read a constant 169-174 ms (240 ms with Wi-Fi off), which suggested phase 1 and then the
  radio start. Neither was it: phase 1 takes 1 ms, Wi-Fi bring-up takes
  127 ms on its own task, and the worst gap began at 3.5 s and 12.6 s, in the middle of
  traffic. It was the SIO task transmitting one 256-byte sector: 259 bytes at 19200 baud is
  about 135 ms, plus 2 ms between each 16-byte chunk (`tx_chunk_gap_us`), and `send()` blocks
  in `flushOutput()` until done. The Atari's next command can arrive before that returns, which
  made the gap look "CMD active". It is not a stall. The metric now runs from the end of one
  poll to the start of the next, so serving a command is excluded and only other work (the
  core tick, services, console) counts. Slow handlers show in `handler`.
- A real stall shows as `react` growing.
- The `Registered ...` lines are buffered stdout and can appear out of order, with late monitor
  timestamps, relative to the early `[N ms]` lines. Trust the `[N ms]` lines for timing.

## Design rules this implies

- Nothing that can take more than a few milliseconds may run on the SIO task, in a handler or in
  startup. Bring-up that touches the radio, network or flash belongs on another task.
- Do not change anything on the D1 boot path without re-running the numbers above; a request
  slower than 16 ms makes the Atari retry or fail.
- A native call that takes long must answer "not ready"/busy at once, not block.

## Open items

- High-speed SIO (HSIO) has not been measured; the numbers above are at 19200 baud.
- FTESTNIO crashes with the BASIC cartridge enabled; boot it with BASIC off (it ends on the
  SELF TEST screen).

## A slow request must not hold back the ACK of its data frame (native endpoint)

An SIO write is command frame, ACK, data frame, **ACK**, COMPLETE. The Atari's drivers wait only a few tens of
milliseconds (about 40 ms in the high-speed driver) for the second ACK, and as long as the command's own
timeout (DTIMLO) for COMPLETE. The native frontend ran the whole request inside `complete_write` and only then
returned `{ACK, COMPLETE}`, so the ACK of the data frame waited for the request. That is invisible while
requests finish in a few milliseconds (a TCP open, an HTTP open, a lookup) and fatal for one that does not: a
`tls://` open blocks the device for 0.5 to 1.5 s while it connects, the Atari's call timed out and retried
(each retry a frame the busy device could not read, so a burst of `UART break detected` when it came back), and
the library reported `$10` for an open that had actually succeeded.

The ESP32 ingress now sends the ACK as soon as the data frame has been checked and its request decoded
(`AtariSioNioFrontend::set_early_ack`), runs the request, then sends COMPLETE after the usual ACK gap.
Frames that are refused (a bad checksum, a request that does not decode) are NAKed as before, never ACKed
first. Without the hook, as on NetSIO, the reply is still `{ACK, COMPLETE}` together. Disk writes are
unchanged (they are short). The `handler` time in `sio.status` now includes the request, so a slow one shows
there.

What this does not do: the request still runs on the SIO task, so a long one stops the ingress from answering
anything else until it finishes (the Atari is waiting for it, so a program that talks only to this endpoint
loses nothing). Making the TLS connect itself asynchronous is the real fix for that.
