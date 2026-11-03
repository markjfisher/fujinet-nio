# Diagnostics & Console

FujiNet-NIO includes a small diagnostic framework in the **core library** plus an optional, shell-like console UI in the **apps**.

Design goals:
- Keep diagnostics **platform-agnostic** and reusable by embedders.
- Keep the console UI **app-only** and platform I/O in `src/platform/*`.
- Avoid `#ifdef` inside core services/devices.

---

## Diagnostic framework (core library)

Public headers live under `include/fujinet/diag/`:
- `diagnostic_types.h`: `DiagResult`, `DiagStatus`, `DiagCommandSpec`, `DiagArgsView`
- `diagnostic_provider.h`: `IDiagnosticProvider` (and the built-in core provider factory)
- `diagnostic_registry.h`: `DiagnosticRegistry`

### Provider model

Providers publish a stable command set and execute commands:
- A provider has a stable `provider_id()` (e.g. `core`, `net`, `fs`).
- Commands are plain strings with a convention of `provider.command` (e.g. `core.stats`).
- Results are returned as a stable `DiagResult`:
  - `text`: human-readable, line-oriented text
  - `kv`: optional key/value pairs for tooling

### Registry model

`DiagnosticRegistry` is a simple aggregator/dispatcher:
- The registry stores pointers to providers (providers own their own state).
- `list_all_commands(out)` gathers `DiagCommandSpec` from all providers.
- `dispatch(args)` tries providers in registration order; providers return `NotFound` when they don’t handle a command.

### Built-in provider: `core`

The built-in provider is created with:
- `fujinet::diag::create_core_diagnostic_provider(core::FujinetCore&)`

It currently exposes:
- `core.info` — version + build profile
- `core.stats` — tick count + registered device count
- `core.mem` — free, lowest-ever and largest-block heap, internal RAM and
  PSRAM separately, and the lowest free stack of the task running the console
  (`fujinet_core` on ESP32). POSIX says it has no statistics. For example:

```
internal_free: 182340
internal_min_free: 150212
internal_largest_block: 110592
psram_free: 7954820
psram_min_free: 5112376
psram_largest_block: 7864320
task: fujinet_core
task_stack_min_free: 2208
```

### Built-in provider: `disk`

The built-in disk provider is created with:

- `fujinet::diag::create_disk_diagnostic_provider(core::FujinetCore&)`

It currently exposes:

- `disk.slots` — list slot state (inserted/ro/dirty/changed), geometry, and mounted image path (`fs:path`)

### Provider: `link`

Created with `fujinet::diag::create_packet_link_diagnostic_provider(channel, fuji)`
when the primary channel is a packet link (`io::PacketLinkChannel`), on POSIX
and ESP32. It exposes:

- `link.status` — state, generation, agreed version, capacity, record timeout,
  stored settings and counters
- `link.set <record_timeout_ms|capacity> <value>` — the record timeout changes
  at once; capacity is stored and applies at the next start
- `link.save` — write `channel.packet_link` into `fujinet.yaml`

See [`packet_link.md`](packet_link.md). On ESP32 the `uart` provider finds the
UART beneath a packet link, so `uart.*` keeps working on packet-link boards.

### Provider: `boot`

Created with `fujinet::diag::create_boot_diagnostic_provider(fuji)` on POSIX and
ESP32, for any build with a `FujiDevice` (it does not need a Channel). It exposes
the `boot:` section of `fujinet.yaml`:

- `boot.get` — `mode`, `config_uri` and `readonly`
- `boot.set <mode|config_uri|readonly> <value>` — change one setting in memory
  (`mode` is `normal` or `config`; `readonly` is `true` or `false`)
- `boot.save` — write the settings into `fujinet.yaml`

The boot disk is read at start-up, so a change applies after a restart. The
default `config_uri` comes from the build profile
(`BuildProfile::defaultBootConfigUri`): the ESP32 Atari NIO build uses
`flash:/boot/atari/autorun.atr`; other builds use `persist:/boot/autorun.img`.
See [`distfiles/esp32-data/boot/README.md`](../distfiles/esp32-data/boot/README.md).

### Provider: `sio` (Atari physical SIO build only)

Created with `fujinet::diag::create_atari_sio_diagnostic_provider(snapshot, reset)`
(`diagnostic_atari_sio.h`) and registered only in the ESP32 Atari NIO build
(`FN_BUILD_ATARI_FUJIBUS_SIO`). The implementation compiles to nothing in every
other build except host tests. The ingress has no logging, because SIO timing is
tight, so this is how to see what the Atari is doing:

- `sio.status` — `cmd_edges` and the CMD line level, `rx_bytes` and UART errors,
  `frames` (complete 5-byte commands) split into D1:-D8:, native (`0x7f`) and other
  devices, the abort counts by reason (`edge`, `timeout`, `overflow`, `rx_error`),
  the last frame seen (device and command decoded), and a `reading:` line naming the
  first thing that looks wrong, in the order the signal travels
  Timing lines (microsecond clock, shown in ms): `boot` gives when the ingress was ready, when
  the first CMD edge arrived and when the first command was processed, so the margin between
  "ready" and "the Atari spoke" is visible; `react` is the worst CMD-release-to-processing
  delay (how fast the loop reacts); `reply` is CMD release to the first reply byte, last and
  worst, with a count of those later than 16 ms (the ACK deadline); `handler` is how long a
  request spent between starting processing and replying; `poll gap` is the longest time the SIO
  task went without polling while the Atari was signalling CMD (idle gaps are not counted);
  `native replies never read` appears when stale replies were discarded. `watch:` lines flag a late ACK, a handler over 100 ms (slow work on
  the SIO task stalls every reply and the Atari times out), a poll gap over 100 ms (a slow
  handler or other work blocked the task) and a
  first CMD edge before the ingress was ready. A native reply the Atari never read is discarded
  by the next Q or W after 2 s, so a timed-out call no longer leaves the endpoint stuck.
  Also shown: `refused` (frames for our devices answered with NAK: how many, the first and the
  latest, with the command named where known), the first eight frames for other devices and the
  latest one (a driver's probe for a device we do not serve), `3F asked` per owner on the `hsio:`
  line, and, on the ESP32, the `second listener` line: a UART on the same RX pin at the speed the
  main one is not using, how many command frames it decoded and how many of those it handled
  because the main UART could not read them.
- `sio.hsio [<0-9> [nofallback]|off]` — high-speed SIO (see [hsio](atari/hsio.md)): with no argument, show the state;
  with an index, answer it to `3F` queries (0 is about 124 kbaud, 6 about 67 kbaud); `off` turns it
  off. `nofallback` keeps the UART at the high speed even when command frames cannot be read (by
  default two in a row toggle between 19200 and the high speed, as the legacy firmware does). Not saved, and it takes effect when the Atari next
  asks. `sio.status` shows the `hsio:` line and, after any unreadable command frame, the first and
  the latest one's bytes and why it was rejected.
- `sio.tune [ack <us>|data <us>|window <ms>|glitch <0|1>|proc <0|1>|reset]` — the dials (`proc 1` pulses PROC when a network session becomes readable and sets Q flag bit 1, see [PROC](atari/proc.md); default off); the timing dials (see [hsio report](atari/hsio-report.md)): the ACK-to-COMPLETE gap (default 250 us), the COMPLETE-to-data gap at high speed (0 us; the legacy firmware uses 1000) and how long a frame heard by the second listener stays usable (10 ms), and `glitch 0|1` (default 1), which ignores CMD edges seen with the line high while a write waits for its data frame. With no argument it shows them; not saved. `sio.status` also shows them, and the measured time per byte of data frames against the wire time, and per command the FujiNet's service time and the Atari's idle time.
- `sio.proc [low|high|pulse [<count> [<low ms> [<high ms>]]]]` — drives the PROC line by hand (assert, release, pulse) and shows it; for proving the wire with PROCTEST, with the Atari idle (see [PROC](atari/proc.md)).
- `sio.save` — store the current `sio.hsio` index and the `sio.tune` values in `fujinet.yaml` (`atari_sio:` `hsio_index`, `ack_gap_us`, `data_gap_us`, `window_ms`, `ignore_write_glitches`, `proc_notify`); they are applied at start-up. The defaults are in `distfiles/atari-fdrive-fujinet.yaml`.
- `sio.baud` — the ESP32 UART baud for each high-speed SIO index against the POKEY rate for PAL
  and NTSC clocks (see [hsio](atari/hsio.md)); run it with the Atari idle, because it retunes the
  UART for a moment. `sio.status` also shows the current UART baud.
- `sio.reset` — zero the counters; do this before powering the Atari. The ingress-ready time is
  kept, since it is not a counter
- `sio.trace` — show the last 16 captured command frames and capture aborts, including CMD state,
  pending write-data length and partial frame state. Consecutive idle edge aborts are collapsed so they
  don't erase the event preceding a stall. Replies record control bytes, size, baud and duration,
  never response payloads. Times are the wrapping 32-bit microsecond clock;
  `sio.reset` clears the trace. This records no network payloads and logs nothing during SIO.
- Reply-time COMMAND noise is classified when the GPIO interrupt is observed,
  rather than clearing pending edges after UART flush. While transmitting, a
  high-level interrupt is ignored only until an assertion is observed. Once low
  is seen, all following edges are preserved, including release. Pending edges
  or an already-low line at reply start disable filtering for that reply.
  This prevents reply noise followed by a genuine next-command assertion from
  becoming a rejected two-edge batch. Ambiguous batches remain rejected; this
  does not infer missing physical transitions or change client retry timing.
  The `CMD edges while we transmitted, dropped as noise` counter includes these
  filtered observations; the total edge counter still includes them as well.
- `sio.trace flight` — retrieve a **128-event first-failure recording**. It runs
  without console output, retaining recent history until an active capture abort
  (edge, timeout, overflow or UART error), ambiguous idle boundary (COMMAND low
  or multiple edges), or a poll gap of at least 16 ms during which COMMAND edges
  occurred. A lone idle edge with COMMAND high is not a trigger. These are
  diagnostic triggers, not proof of a lost application request.
  After reset, triggering waits for both two seconds and 32 history events;
  failures during this warm-up are recorded without freezing. This avoids
  capturing console/reset disruption without useful preceding history.
  After triggering it records up to 16 following events, or one second, then
  freezes. With a full history this retains 111 preceding events, the trigger,
  and 16 following events. With less traffic/history it retains fewer. Idle
  aborts are not coalesced in this recording. `warming up`, `armed`, `collecting` and `frozen`
  indicate its state. Reads do not rearm it; **`sio.reset` clears and rearms it**.
  The frozen recording remains unchanged while traffic continues or while it is
  printed, so there is no need to time the console command to a short pause.
  Reset during normal application operation to avoid capturing boot probes.
- Trace metadata now includes `last_edge_us` (latest ISR edge timestamp),
  `edge_batch` (observed edges consumed at this decision; poll-gap events report
  the change since the previous poll), and `poll_gap_us` (end-of-previous-poll
  to start-of-current-poll when COMMAND edges occurred). Event 8 marks a polling
  gap; event 9 records the early data ACK separately from COMPLETE. `record`
  indexes identify retained events; the frozen header names the triggering index.
  Events 10–12 record poll-observed COMMAND edge batches, capture start, and
  capture release, only in the flight recorder. They are not per-edge ISR
  samples: a batch can contain multiple transitions; `low` is the sampled level.
  A separate 16-observation IRQ snapshot is captured at the trigger and printed
  after the 128-event history. It includes each sampled level, timestamp,
  filtering decision, and reply state (0 outside, 1 filtering, 2 preserving).
  It includes filtered interrupts too. This is additional history, not a
  reduction of the 128-event ring. Atomic versioned slots allow bounded reading
  without blocking the ISR; an overwritten/inconsistent slot is omitted.
  Sequence numbers continue across resets. Levels are ISR observations, not
  proof of physical transitions; delayed/coalesced interrupts remain possible.
  Storage adds roughly 400 bytes per ingress/recorder combined. No allocation
  or console output occurs in the interrupt. Capture acceptance is unchanged.
  Event 14 marks a tolerated repeated-high observation: collection has already
  seen release, the line remains high, and exactly one new ISR observation is
  available with no intervening low or filtered observation. Collection continues
  under its existing 50 ms deadline; size, checksum and UART-error handling are
  unchanged. Multiple edges, a new assertion, or missing observation history
  remain ambiguous and are rejected. The ISR samples cannot rule out a pulse
  shorter than interrupt observation, so this is a bounded recovery policy,
  not proof that every repeated-high interrupt is electrically spurious.
  Event 15 marks idle high-then-low recovery: no command or write is pending,
  exactly two edges were consumed, COMMAND is currently low, and the latest two
  coherent IRQ observations are unfiltered high then low, both outside reply
  transmission. The leading idle high is discarded and the low starts normal
  collection. Low-low, high-high, missing history, active collection and pending
  writes retain the existing rejection behavior. No elapsed-time grace window
  is added. As with event 14, this is based on sampled levels, not proof that
  the electrical line had no shorter unobserved pulse.
  Event 16 recovers the min-009 reply handover: exactly two low observations,
  the first in reply filtering state and the second in preservation state,
  separated by at most 200 microseconds, with capture idle and no pending write.
  The assertion is retained. This interval is shorter than a complete command
  at the supported maximum HSIO rate. Other low-low batches remain rejected.
  It is a bounded sampled-level recovery, not a claim to identify physical noise.
  ESP-IDF's GPIO driver clears edge interrupt status before the callback; no
  missing acknowledgement was found, and no SDK or GPIO interrupt-mode change
  was made.
  Event 17 triggers on 200–1000 ms between consecutive decoded native S/T
  commands, even without a capture abort. `gap_us` is the command-to-command
  interval, not server execution time. User/application pacing can also trigger
  this heuristic; it is evidence to inspect, not an error classification.
  Non-stream commands break the sequence and `sio.reset` clears its baseline.
  Abort `frame=` is the previous/partially collected frame, not necessarily the
  command lost. A poll-gap event has no command frame. This is metadata, not a
  logic-analyser capture; multiple edges can occur before a GPIO level is sampled.
  Shared platform-independent storage/trigger logic is host-tested; physical
  edge/poll metadata and early ACK instrumentation are ESP32 ingress hooks.

  Hardware capture procedure:

  1. Boot and enter the application, then run `sio.reset` once.
  2. Let it run without console commands until pauses occur; wait another second.
  3. Run `sio.trace flight`, then `sio.status`, and save the complete output.
  4. Repeat with `sio.reset` if necessary. Do not spam live dumps: console output
     can itself delay polling. A frozen dump can be read repeatedly later.

  The 128-entry recorder is heap-owned (about 5 KiB plus allocation bookkeeping),
  rather than embedded in the repeatedly copied stats structure. A frozen snapshot
  shares immutable storage. Reset allocates a new recorder before releasing the
  old one, so peak reset memory includes two recorders; snapshots held by a caller
  keep their old recording valid. There is no allocation or logging per event and
  no additional task. Formatting a dump uses temporary heap text proportional to
  the bounded 128-event output. The legacy live ring remains 16 entries for small
  ordinary status snapshots. This changes diagnostics only, not SIO framing,
  timeout/retry policy, the small-stream protocol or network services.
- `sio.status` reports buffered bytes discarded from each UART after draining pending events. Bytes
  arriving between that drain and the hardware flush may not be counted; `sio.reset` zeros these counts.

The timings are measured at the existing send and poll points; nothing is logged on the SIO
path.

A quick reading: `cmd_edges: 0` means the CMD pin never changes (the Atari is not
sending, or CMD is wired to a different pin than `pinmap.cpp` says); edges but
`rx_bytes: 0` points at the data-in pin; bytes but `frames: 0` points at the aborts.
Pair it with `disk.stats` to see whether sector requests reach the disk.
### Provider: `hotsync`

Registered by `fujinet::hotsync::start_hotsync()` in builds with HotSync
(`FN_WITH_HOTSYNC`; see [`hotsync.md`](hotsync.md)), on POSIX and ESP32,
whether or not `hotsync.enabled` is on. It exposes:

- `hotsync.status` — whether NetSync is listening, the cradle's state and
  counters (windows, FujiBus requests, stray bytes, partial frames dropped
  after stalling, refusals, dropped writes), the settings, sync counts, and
  the source, user, time and result of the last sync
- `hotsync.get` — the stored `hotsync:` settings
- `hotsync.set <enabled|port|cradle|root|user_name|backup> <value...>` —
  change a stored setting (`enabled` and `cradle` are `0` or `1`; `port` 0
  turns NetSync off; `backup` is `none`, `flagged` or `all`; `user_name` may
  contain spaces, up to 40 characters)
- `hotsync.save` — write the `hotsync:` settings into `fujinet.yaml`
- `hotsync.fetch <http://host[:port]/file.prc> [name] [sha256=<hex>]` —
  download a `.prc`, `.pdb` or `.pqa` over plain HTTP into `install/` for the
  next sync (needs HotSync on); with `sha256=` it is queued only if its
  SHA-256 matches

`get`, `set` and `save` need the FujiDevice. The service reads its settings at
start-up, so a change takes effect after `hotsync.save` and a restart. See
[`hotsync.md`](hotsync.md).

### Provider: `net`

Created with `fujinet::diag::create_network_diagnostic_provider(core, wifi_ctx)`.
It exposes:

- `net.sessions` — one line per active network session
- `net.close <handle|all>` — close a session handle (or all)
- `net.translation.get` — content translation settings: the Image
  translator's pixel cap, live, stored and the platform default
- `net.translation.set image_max_pixels <pixels|default>` — change the cap
  for translations configured from now on (1 to 67108864, which is
  8192x8192); with a FujiDevice it is also stored, and `default` stores 0
- `net.translation.save` — write `translation` (`image.max_pixels`) into
  `fujinet.yaml` (needs the FujiDevice from the Wi-Fi context)
- `net.translation.stats` — totals since start-up and the last 8
  translations, newest first, with time, sizes, stack and heap used
- `net.wifi.*` — scan, status, get, set and save (when a Wi-Fi context is given).
  `net.wifi.status` includes `join_trial`: idle, connecting, succeeded, or failed
  with previous-network restoration attempted. Scan and set report busy during a trial.

`net.sessions` starts with an `active_sessions: N` line, then one line per
active session, each reading:

```
active_sessions: 1
handle=0x0102 method=1 flags=0 awaiting_body=0 body=0/0 completed=1 url=http://example.com/a.png translation=image selector=w=640,h=400,colors=16 ready=1 translated=18432
```

`translation` is `none`, `json`, `xml`, `rss` or `image`. For anything but
`none` the line continues with `selector` (the JSON Pointer, or the image
selector), `ready` (`1` once the body has been translated) and `translated`
(size in bytes of the translated view; `0` until it is ready). The selector
is printed as given and may contain `=` and `,`; it sits before `ready=`, so
parse from the right.

`net.translation.get` prints three lines; `stored_image_max_pixels` is `default`
when `fujinet.yaml` has 0, and is left out without a FujiDevice:

```
image_max_pixels: 1048576
stored_image_max_pixels: default
platform_default_image_max_pixels: 1048576
```

See [Image translation](network_device_protocol.md#image-translation-type-4)
for what the cap limits.

`net.translation.stats` reads (one translation per line, newest first):

```
translations: 3
failed: 1
type=image status=unsupported ms=5 body=223158 out=0 stack_used=2376 heap_peak=68608 heap_free_before=8173907 heap_largest_block=7602176 : 740x1215 is 899100 pixels, over the cap of 490000
type=image status=ok ms=2310 body=19234 out=27082 stack_used=21344 heap_peak=1203456 heap_free_before=7954820 heap_largest_block=7864320 : 700x500 -> 319x228 ilbm bits=4 colors=16
type=json status=ok ms=3 body=386 out=24 stack_used=0 heap_peak=0 heap_free_before=0 heap_largest_block=0
```

- `ms`: time the translation blocked the device; `body`/`out`: bytes in and
  out.
- `stack_used`, `heap_*`: measured by the platform's large-stack runner
  (`platform::run_with_large_stack()`) around an image conversion, against
  its 48 KB stack (`ImageContentTranslator::kConvertStackBytes`).
  `heap_peak` is how far free heap (internal and PSRAM together) fell below
  `heap_free_before` during the conversion. `0` means not measured: JSON
  runs on the caller's stack, and POSIX measures only the stack. (The
  figures above are illustrative.)
- After ` : `, the translator's own one-line description: for an image the
  source and output size and format, or why it failed (over the pixel cap,
  not an image, out of memory).

---

## Console engine (app-only)

The console engine is pure parsing/dispatch logic:
- Header: `include/fujinet/console/console_engine.h`
- Implementation: `src/app/console_engine.cpp`

It depends on:
- a `diag::DiagnosticRegistry&`
- an `IConsoleTransport&` that provides byte I/O with timeouts (the engine includes a small line editor + history)

### Console commands

- `help`
- `exit` / `quit`
- `<provider> <command> [args...]`
  - Example: `core stats` → dispatches `core.stats`
  - Example: `net close 0x0102` → dispatches `net.close 0x0102`
- `<provider>.<command> [args...]`
  - Example: `core.stats`
- `list`
- `dump`

### Prompt + echo

Many PTY/serial clients do not echo locally. The console prints a simple prompt:

```
> 
```

The console does **not** currently echo your input characters; use a client with local echo if you want to see what you type.

---

## Platform transports (platform glue)

Platform I/O implementations live in `src/platform/<posix|esp32>/` and are selected by `create_default_console_transport()`.

### POSIX

POSIX defaults to a **dedicated console PTY**, separate from the FujiBus PTY used for normal traffic.

At startup you should see two different PTYs printed:
- Console:
  - `[Console] PTY created. Connect diagnostic console to: /dev/pts/<N>`
- FujiBus (normal traffic):
  - `[PtyChannel] Created PTY. Connect to slave: /dev/pts/<M>`

Important: **Do not connect your FujiBus tooling to the console PTY** (and vice-versa).

#### POSIX: choosing console transport (PTY vs stdio)

- Default: PTY console
- Force stdio console (stdin/stdout of the process):

```bash
FN_CONSOLE=stdio ./fujinet-nio
```

#### POSIX: connecting with picocom (recommended)

Example (replace with appropriate port from the app startup):

```bash
picocom -q --echo --omap crlf --imap lfcrlf /dev/pts/8
```

Notes:
- Replace `/dev/pts/8` with the **console PTY** printed by `[Console] ...`, not the FujiBus PTY.
- `--omap crlf` / `--imap lfcrlf` help keep output aligned and make Enter behave naturally.

#### POSIX: connecting with screen (works, but less friendly)

```bash
screen /dev/pts/<N>
```

To exit:
- Detach: `Ctrl-A` then `d`
- Quit: `Ctrl-A` then `\` then `y`

### ESP32

On ESP32 builds, FujiBus commonly uses **USB CDC** for normal traffic (BuildProfile: `UsbCdcDevice`).

Console transport note:
- The console must **not share** the same USB CDC stream as FujiBus, or it will corrupt the wire protocol.
- The console transport is configurable via **sdkconfig/Kconfig** (see below).

Typical usage:
- Use PlatformIO/ESP-IDF serial monitor (UART0) to interact with the console.
- The console prompt and commands are the same as POSIX.

Future direction:
- Use **two TinyUSB CDC ACM ports** (multi-CDC) so the host sees two separate `/dev/ttyACM*` devices:
  - FujiBus on one port (default: ACM0)
  - Console on a different port (default: ACM1)

#### ESP32: configuration (sdkconfig)

FujiNet-NIO provides Kconfig options under `menuconfig` → **FujiNet-NIO**:

- `CONFIG_FN_CONSOLE_ENABLE`
  - Enable/disable the diagnostic console in the ESP32 app.
- `CONFIG_FN_CONSOLE_TRANSPORT_UART0` / `CONFIG_FN_CONSOLE_TRANSPORT_USB_CDC`
  - Select whether console runs on UART0 or TinyUSB CDC ACM.
- `CONFIG_FN_FUJIBUS_USB_CDC_PORT`
  - Which TinyUSB CDC ACM port FujiBus uses (default: `0`).
- `CONFIG_FN_CONSOLE_USB_CDC_PORT`
  - Which TinyUSB CDC ACM port the console uses (default: `1`).

To expose **two** CDC ACM ports from TinyUSB, set:
- `CONFIG_TINYUSB_CDC_ENABLED=y`
- `CONFIG_TINYUSB_CDC_COUNT=2`

Notes:
- If `CONFIG_TINYUSB_CDC_COUNT<2`, a USB-CDC console cannot be dedicated; FujiNet-NIO will fall back to UART0 when configured for USB CDC.
- The exact host node numbering (`/dev/ttyACM0`, `/dev/ttyACM1`, `/dev/ttyACM2`) is not guaranteed to be stable across replug/boot; consider udev rules for stable symlinks if needed.

#### Linux: stable `/dev/` names with udev (recommended)

When you expose multiple serial interfaces (e.g. USB-Serial-JTAG + TinyUSB CDC ACM0 + TinyUSB CDC ACM1),
Linux may assign `/dev/ttyACM*` numbers differently across boots or re-plugs.

To make tooling reliable, create stable symlinks like:
- `/dev/fujinet-fujibus`
- `/dev/fujinet-console`

##### 1) Inspect the attributes for each port

Plug the device in, then for each candidate port (example: `/dev/ttyACM1`, `/dev/ttyACM2`):

```bash
❯ udevadm info -n /dev/ttyACM1 -q property | grep -E 'ID_VENDOR_ID|ID_MODEL_ID|ID_USB_INTERFACE_NUM'
ID_MODEL_ID=4002
ID_VENDOR_ID=303a
ID_USB_INTERFACE_NUM=00

❯ udevadm info -n /dev/ttyACM2 -q property | grep -E 'ID_VENDOR_ID|ID_MODEL_ID|ID_USB_INTERFACE_NUM'
ID_MODEL_ID=4002
ID_VENDOR_ID=303a
ID_USB_INTERFACE_NUM=02
```

Tip: quick view via sysfs for the ID_USB_INTERFACE_NUM, it equates to the following path:

```bash
cat /sys/class/tty/ttyACM1/device/bInterfaceNumber
```

For multi-CDC, a common pattern is:
- CDC ACM0 → interface number `00`
- CDC ACM1 → interface number `02`

…but **verify on your system** using the commands above.

Alternate way to check the values is via journalctl logs:
```
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: new full-speed USB device number 49 using xhci_hcd
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: New USB device found, idVendor=1a86, idProduct=55d3, bcdDevice= 4.45
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: New USB device strings: Mfr=0, Product=2, SerialNumber=3
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: Product: USB Single Serial
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: SerialNumber: 5875011611
Jan 06 19:03:29 archy kernel: cdc_acm 7-1.3.3:1.0: ttyACM0: USB ACM device
```

##### 2) Create udev rules

Create a rules file:

```bash
sudoedit /etc/udev/rules.d/99-fujinet-nio.rules
```

Template (replace VID/PID and interface numbers with what you observed):

```udev
# FujiNet-NIO (TinyUSB CDC ACM)
# Replace ID_VENDOR_ID/ID_MODEL_ID and ID_USB_INTERFACE_NUM to match your device.

# When TinyUSB is configured with 2 ports, the ID_MODEL_ID is 4002
ACTION=="add", SUBSYSTEM=="tty", KERNEL=="ttyACM*", \
  ENV{ID_VENDOR_ID}=="303a", ENV{ID_MODEL_ID}=="4002", ENV{ID_USB_INTERFACE_NUM}=="00", \
  SYMLINK+="fujinet-fujibus"

ACTION=="add", SUBSYSTEM=="tty", KERNEL=="ttyACM*", \
  ENV{ID_VENDOR_ID}=="303a", ENV{ID_MODEL_ID}=="4002", ENV{ID_USB_INTERFACE_NUM}=="02", \
  SYMLINK+="fujinet-console"

# When TinyUSB is configured with 1 port, the ID_MODEL_ID is 4001
ACTION=="add", SUBSYSTEM=="tty", KERNEL=="ttyACM*", \
  ENV{ID_VENDOR_ID}=="303a", ENV{ID_MODEL_ID}=="4001", ENV{ID_USB_INTERFACE_NUM}=="00", \
  SYMLINK+="fujinet-fujibus"

# rs232 fujinet, 1 port, the ID_MODEL_ID is 1001
ACTION=="add", SUBSYSTEM=="tty", KERNEL=="ttyACM*", \
  ENV{ID_VENDOR_ID}=="303a", ENV{ID_MODEL_ID}=="1001", ENV{ID_USB_INTERFACE_NUM}=="00", \
  SYMLINK+="fujinet-rs232"

# atari fujinet
ACTION=="add", SUBSYSTEM=="tty", KERNEL=="ttyUSB*", \
  ENV{ID_VENDOR_ID}=="10c4", ENV{ID_MODEL_ID}=="ea60", ENV{ID_USB_INTERFACE_NUM}=="00", \
  SYMLINK+="fujinet-atari"
```

Notes:
- The example `303a` VID is Espressif; your PID may differ depending on descriptor settings.
- `ID_USB_INTERFACE_NUM` matching is usually the most reliable way to separate ACM0 vs ACM1 on the same USB device.

##### 3) Reload rules

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger --sysname-match=tty
```

Then unplug/replug the device, and verify:

```bash
ls -l /dev/fujinet-*
```


