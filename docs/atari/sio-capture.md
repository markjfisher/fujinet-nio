# Atari physical SIO capture and recovery

The ESP32 physical-SIO ingress collects COMMAND transitions and UART bytes,
then hands complete frames to the existing disk/native router. Its capture
state machine and recovery rules live in
`include/fujinet/io/atari_sio_transaction_boundary.h`; GPIO/UART hooks live in
`src/platform/esp32/atari_sio_ingress.cpp`.

## Why recovery is needed

Atari hardware testing with Bounce World's polled 19200-baud small-stream
transport exposed repeated same-level COMMAND interrupt observations. Rejecting
every extra observation abandoned otherwise deliverable commands and caused
roughly 300 ms client retries. A separate state-machine bug required an idle
poll after an already-released idle-high observation; when a new assertion woke
the task first, that command was discarded too. Sampling the pin again during
abort cleanup could similarly mistake a new assertion for the old failure.

ESP-IDF clears edge interrupt status before calling the per-pin handler. No
missing acknowledgement was found. An interrupt's sampled level is not a
logic-analyser measurement: delayed/coalesced interrupts or short physical pulses
remain possible. These policies provide bounded recovery from observed sequences;
they do not establish the electrical source of every extra interrupt.

## Capture contract

- Reply-time high observations are filtered at interrupt observation time, until
  low is seen. Subsequent edges are preserved. Already-pending boundaries or a
  low line at reply start disable filtering for that reply. Pending assertions
  are never cleared wholesale after UART flush.
- A single idle-high observation leaves capture ready for the next assertion.
  Abort-entry state controls this decision; an assertion during cleanup remains
  queued. Active/ambiguous aborts retain the discard-until-release barrier.
- Recovery uses coherent, version-checked IRQ history. Missing history or a
  concurrent overwrite fails closed. Multiple edges are not generally accepted.
- Existing command/write deadlines, frame-size/checksum handling, UART-error
  checks and downstream router validation remain in force.

| Event | Recovery preconditions | Action |
| --- | --- | --- |
| 14 | Command already released; line high; exactly one further unfiltered high observation | Continue collecting under the original deadline |
| 15 | Idle, no pending write; exactly two observations, high then low, unfiltered and outside replies; line low | Ignore leading idle high and start command capture |
| 16 | Idle, no pending write; exactly two lows, first during reply filtering and second in preservation state; at most 200 us apart | Retain assertion as one start boundary |
| 18 | Collecting an unreleased command; exactly two observations since assertion, both unfiltered highs outside replies | Treat as release followed by event-14 recovery |

The event-16 bound is shorter than a complete command frame at the supported
maximum HSIO speed. Other low-low sequences remain rejected. Event 18 makes the
same observed release sequence work whether processed in one poll or two.

Recovery is confined to physical Atari SIO command capture. It introduces no
protocol fields, client API changes, network-service behavior, U1MB table writes,
DLI suppression, or changes to advertised HSIO settings. POSIX NetSIO and other
platform transports do not acquire the ESP32 GPIO behavior. Shared capture tests
exercise the same decision code used on hardware.

## Flight recorder

`sio.trace` prints the compact live 16-event trace. `sio.trace flight` prints a
separate frozen 128-event history plus the last 16 IRQ observations at the trigger.
Both contain control metadata, not network payloads.

After `sio.reset`, recording warms up for at least two seconds and 32 events.
It then triggers on an active capture abort, an ambiguous idle boundary, a
polling gap of at least 16 ms with COMMAND activity, or a 200–1000 ms interval
between consecutive decoded native S/T commands. The latter is a diagnostic
heuristic: application pacing can also cause it. Non-stream commands break that
interval sequence; reset clears its baseline.

After triggering, the recorder collects up to 16 more events or one second,
then freezes. With full history that retains 111 preceding events, the trigger,
and 16 following events. It stays frozen until reset; output can therefore
refer to startup rather than the later animation if reset occurred too early.

| Event | Meaning |
| --- | --- |
| 1 | Decoded command frame |
| 2–5 | Edge, timeout, overflow, or UART-error abort |
| 6 | Idle edge observation; not necessarily a lost command |
| 7, 9 | Reply and separate early data ACK |
| 8 | Polling gap with COMMAND activity |
| 10–12 | Poll-observed edge batch, capture start, capture release |
| 14–16, 18 | Successful boundary recoveries described above |
| 17 | Stream command interval; `gap_us` is not server execution time |

`record` numbers identify retained events. `last_edge_us` is the latest IRQ
clock reading; `edge_batch` is the number observed at the decision; `low` is the
sampled level. Abort `frame=` can be the previous or partially collected frame.
It does not identify a failed command when `used=0`. Times are wrapping 32-bit
microseconds. IRQ rows include sequence, sampled level, filtering decision and
reply state: 0 outside, 1 filtering, 2 preserving. IRQ sequence numbers continue
across resets. Inconsistent/overwritten snapshot slots are omitted.

## Cost and observation procedure

The event recorder occupies approximately 5 KiB of heap plus bookkeeping and
its 192-byte trigger IRQ snapshot. The live IRQ observation ring adds about
200 bytes per ingress. No allocation, console logging or new task runs in the
ISR or per-event recording path. Synchronization uses 32-bit atomics; the ESP32
build does not promise `is_always_lock_free`. ESP-IDF supplies native operations
or ISR-safe critical-section fallbacks according to target/toolchain options.
The snapshot reader performs bounded version checks and does not spin waiting
for the writer. A port must provide ISR-safe atomics; generic hosted fallbacks
are not automatically suitable.
Snapshots share immutable frozen storage. Reset allocates a replacement before
releasing the old recorder, so transient memory can include two recorders;
readers holding an old snapshot keep it alive. Formatting output uses additional
bounded temporary heap. The live ring keeps ordinary status snapshots small.

1. Enter the animation, then run `sio.reset` once.
2. Leave the console idle during the measurement period.
3. After a pause, wait a second, then save `sio.trace flight` and `sio.status`.
4. Rearm only when starting a fresh observation period.

**Printing diagnostics can itself block SIO polling.** Hardware testing observed
roughly 1–2 seconds while dumping a trace. A maximum-latency rise during console
output is not evidence of an undisturbed gameplay stall. Do not repeatedly dump
traces to try to catch a brief pause. An armed response means no trigger fired,
not that every possible client/network delay was absent. The lost-frame counter
covers abandoned active collections, not every rejected idle boundary.

## Validation

Host regression tests cover the hardware-observed sequences, idle cleanup races,
missing/ambiguous history, state and timing bounds, malformed frames, UART errors,
clock wrap, recorder freezing and immutable snapshot lifetime. Run the
`fujibus-pty-debug` CTest suite and build the Atari v1 16MB ESP32 profile.

The user accepted the final hardware build after sustained Bounce World running
with no major recurring stalls. Earlier undisturbed testing held around 4.5 new
world steps/s and maximum fetch time at 25 PAL video ticks for over six minutes.
A PC sleep/network disruption produced a blip; uninterrupted service during
server/network loss is not promised. This is workload-specific acceptance, not
an assertion that every Atari expansion or electrical setup has been tested.

Upload firmware from the firmware repository with `./build.sh -u`.
