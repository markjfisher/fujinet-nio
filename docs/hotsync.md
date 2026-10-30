# Palm HotSync

FujiNet NIO can be the desktop end of a Palm OS HotSync. A Palm syncs with it
to receive queued apps and to have its databases backed up, the jobs Palm
Desktop used to do.

A Palm can sync two ways:

- **Cradle:** a Palm in a serial cradle on the FujiBus serial line (the
  FN-RS232 DB-9, or a POSIX serial port), by pressing its HotSync button. The
  same line carries FujiBus for Palm apps; see [Cradle](#cradle).
- **Network HotSync (NetSync)** on TCP 14238, from a Palm's Network HotSync or
  from `scripts/fake_palm.py`.

A sync identifies the Palm's user (a Palm that has never synced is given a
name and a new user ID), installs queued `.prc`, `.pdb` and `.pqa` files,
backs up databases, and stamps the Palm as last synced with FujiNet.

Not yet: copying a calendar into the Date Book.

## Builds

HotSync is in builds with `FN_WITH_HOTSYNC`: on by default for the POSIX
`fujibus-pty-*` and `fujibus-rs232-*` presets and on the FN-RS232 ESP32
boards. `-DFN_WITH_HOTSYNC=OFF` leaves it out of a POSIX build and `=ON` adds
it to another. Other builds carry none of its code; see
[build_feature_matrix.md](build_feature_matrix.md).

## Turning it on

In those builds HotSync is off until it is turned on. Add a `hotsync:` block to
`fujinet.yaml` and restart:

```yaml
hotsync:
  enabled: true
  port: 14238          # NetSync TCP port; 0 turns NetSync off
  cradle: true         # a Palm cradle shares the FujiBus serial line (see Cradle)
  root: /hotsync       # folder on the persistent filesystem
  user_name: FujiNet   # given to a Palm that has never synced
  backup: flagged      # none | flagged | all
```

Or from the diagnostics console:

```
hotsync.set enabled 1
hotsync.set cradle 1
hotsync.set user_name Andy D
hotsync.save
```

`hotsync.get` shows the stored settings. They are read at start-up, so a
change takes effect after a restart.

`backup` chooses what a sync copies off the Palm:

- `none`: nothing.
- `flagged`: databases whose backup bit is set, as Palm Desktop does.
- `all`: every RAM database.

"Unsaved Preferences" is never backed up; every Palm rebuilds it.

## Files

The folder lives on the persistent filesystem (`host` on POSIX, `sd0` or
`flash` on ESP32), under `root`:

| Folder | Contents |
|---|---|
| `install/` | Files to install at the next sync. FujiNet creates it at start-up. |
| `installed/` | Files move here once installed. |
| `incoming/` | `hotsync.fetch` downloads, until complete. |
| `backup/<user>/` | Databases backed up from the Palm, as `.pdb` or `.prc` |

Copy a file into `install/` through any route that reaches the persistent
filesystem: the host folder on POSIX, the SD card, or a FileDevice write from a
FujiNet client. File and user names are made FAT-safe: characters other than
letters, digits, space, `-`, `_` and `.` become `_`.

Or download it from the console. Serve the files from a computer on the
network, then fetch each into the queue:

```
python3 -m http.server 8000                     # on the computer, in the folder with the .prc
hotsync.fetch http://192.168.1.5:8000/FujiNetTest.prc     # on the FujiNet console
```

`hotsync.fetch` takes plain `http://` only (no TLS, no redirects) and a name
ending in `.prc`, `.pdb` or `.pqa`, from the URL or given as a second argument.
The file lands in `incoming/` first and moves to `install/` when complete.

Give it the file's SHA-256 and it queues the download only if it matches:

```
shasum -a 256 FujiNetTest.prc                   # on the computer
hotsync.fetch http://192.168.1.5:8000/FujiNetTest.prc sha256=<the 64 hex digits>
```

A database larger than 1 MB is neither installed nor backed up, so it never
has to be held in memory. On ESP32 buffers that large come from PSRAM.

### Sharing the filesystem

The service writes from its own task while the core may be using the same
filesystem for FileDevice and DiskDevice. That is safe on the local
filesystems HotSync uses (host, `sd0`, `flash`): `fs::IFileSystem` documents
why, and HotSync refuses a network filesystem. Two things still follow from
sharing it:

- Two writers to one file interleave. A client copying a file into `install/`
  while a sync reads it can hand the sync part of a file, which the sync then
  refuses as corrupt. `hotsync.fetch` writes to `incoming/` and renames, for
  this reason; a FileDevice client should copy to another name and rename, or
  write the file's header last.
- A directory listing taken during a sync may or may not show the files it
  adds.

`tests/test_hotsync.cpp` runs 100 syncs while another thread writes, reads,
lists and removes files on the same filesystem.

## Security

HotSync trusts its network, as Palm Desktop did:

- NetSync listens on TCP `port` on every interface whenever HotSync is on, and
  asks for no password. Anything that can reach it and speaks NetSync is given
  the queued apps and can write backups (each at most 1 MB) under `backup/`.
  Keep the FujiNet on a trusted network, or set `port: 0` to turn NetSync off
  and keep the cradle.
- `hotsync.fetch` uses plain HTTP with no TLS, so anything on the network path
  can substitute the file, and the next sync installs it on the Palm. Fetch
  from a computer on the same trusted network, and pass `sha256=` with a digest
  you got from the file's publisher. HTTPS through the firmware's network
  backend is possible future work.
- The cradle needs physical access to the Palm and the FujiNet.

## Syncing a Palm

In a cradle: press the cradle's HotSync button, or open HotSync and tap the
Local Sync icon (Direct Serial). The sync starts within a few seconds.

Over the network: open HotSync, choose **Modem Sync Prefs** → **Network**, set
the primary PC address to the FujiNet's IP address, and tap the Modem Sync
icon.

## Cradle

A HotSync opens at 9600 baud; FujiBus runs at the bus rate, 115200 by default.
At the wrong rate the UART cannot see a HotSync start, so FujiNet listens for
one now and then, and FujiBus is off the line while it does:

- With `hotsync.cradle` on, `hotsync::CradleChannel` owns the FujiBus line.
  FujiBus reads and writes it as before; the HotSync service borrows it.
- Every 3 seconds the service opens a window: the line drops to 9600 baud for
  1.5 s while it listens for a Palm's WAKEUP, then goes back to the bus rate.
  A WAKEUP keeps the line for the whole sync, including the change to the
  faster rate the Palm accepts; afterwards the bus rate is restored and bytes
  left at the other rate are dropped.
- A Palm running an app is not HotSyncing: each FujiBus packet keeps windows
  closed for 60 s. A SLIP frame counts as a packet only if the FujiBus
  transport would take it: `CradleChannel` decodes it with `decodeSLIP` and
  checks it with `FujiBusPacket::fromRaw`, so the two cannot disagree.
- Bytes that are not FujiBus, with no packet in the last second, end that wait
  at once: a Palm whose HotSync button was pressed sends a WAKEUP, which reads
  as such bytes at the bus rate.
- While the line is lent FujiBus requests get no answer, and the request after
  a sync can be lost to the Palm's leftover WAKEUP bytes; Palm apps retry.

**Other FujiBus clients on the line.** With `hotsync.cradle` on, after 60 s
without a FujiBus packet the line is deaf to FujiBus for 1.5 s of every 3 s,
and a request sent in that time is lost. Turn it on only where the clients
retry: the Palm library on `fujinet-nio-lib` reclaims the line after an idle
spell with a FujiDevice GetInfo, which is harmless to repeat. Leave it off on a
line shared with a computer whose FujiBus client does not retry.

The line must be able to change rate: the ESP32 UART (FN-RS232) or a POSIX
serial port (`fujibus-rs232-*` presets). A Palm cradle never grants RTS/CTS
flow control, so leave `channel.uart.flow_control` off. On the FN-RS232 the
cradle goes on the DB-9 through a null-modem adapter.

### Testing with an emulator

CloudpilotEmu can bridge its cradle serial port to a host pty. A socat pty pair
joins it to a POSIX build:

```sh
socat -d -d -x pty,raw,echo=0,link=palm-tty pty,raw,echo=0,link=fn-tty &
FN_SERIAL_PORT=$PWD/fn-tty FN_SERIAL_BAUD=115200 build/fujibus-rs232-debug/fujinet-nio &
cloudpilot-emu palm.img --serial $PWD/palm-tty
```

with `hotsync.enabled` and `hotsync.cradle` on in `fujinet-data/fujinet.yaml`.
Press the emulated cradle button (`button cradle click` on CloudpilotEmu's
command line). A pty carries bytes whatever the baud rate, so this checks the
hand-off and the protocol, not the rate changes.

Without a Palm, `scripts/fake_palm.py` plays one:

```sh
./scripts/fake_palm.py localhost --state palm.json
```

It keeps its user and databases in the state file, so a second run is a
returning Palm. A new state file holds one flagged database, "FujiNet Notes",
so the first sync with `backup: flagged` backs it up.

## Diagnostics

`hotsync.status` shows whether the service is on and listening, its settings,
how many syncs have run, and the result of the last one. `rejected` counts
connections that never completed the NetSync handshake, such as a port scan,
and Palms that woke but did not finish the cradle handshake; they are not
syncs. With a cradle it adds the cradle's state (`app` while a FujiBus host
holds the line, `listening` during a window, `syncing`, `idle`) and counters:
windows opened, FujiBus packets seen, stray bytes, windows refused while an
app was active, and FujiBus writes dropped while the line was lent. On ESP32,
`task_stack_min_free` is the least free stack the HotSync task has had, in
bytes, out of its 8192:

```
enabled: 1
listening: 1
port: 14238
cradle: listening
cradle_windows: 18
cradle_fujibus_packets: 2
cradle_stray_bytes: 273
cradle_refused: 2
cradle_dropped_writes: 0
root: /hotsync
user_name: FujiNet
backup: flagged
syncing: 0
syncs: 1
failures: 0
rejected: 0
last_source: cradle
last_time: 2026-10-18 13:02:12
last_user: Test Palm
last_result: ok
last_installed: 1
last_install_failures: 0
last_backed_up: 1
last_backup_failures: 0
task_stack_min_free: 4076
```

The `task_stack_min_free` above is from an FN-RS232 ESP32-S3 after a cradle
sync that backed up ten databases: a sync uses about 4 KB of the task's 8 KB.

## Design

- The protocol stack lives in `fujinet::hotsync` (`include/fujinet/hotsync/`,
  `src/lib/hotsync/`) and depends on no platform, bus or device:
  - `Slp`, `PadpTransport` (with CMP): the serial stack.
  - `NetSyncTransport`: the network framing and handshake.
  - `DlpClient`: Desktop Link Protocol commands.
  - `PalmDatabase`: `.pdb` and `.prc` files.
  - `Session`: one sync, over injected `DlpTransport` and `Storage` interfaces.
  - `CradleChannel`: the `io::Channel` adapter that shares the FujiBus line
    (above). It changes rate through `io::BaudControl`, which the ESP32
    `UartChannel` and the POSIX serial channel implement beside
    `io::Channel`; HotSync code never touches the UART itself.
- `HotSyncService` listens for NetSync through `net::ITcpSocketOps` and runs
  the cradle windows, one sync at a time. A sync waits on the Palm for seconds
  at a time and runs for minutes, so the service runs on a task of its own
  (`platform::start_service_task`, an 8 KB stack in internal RAM on ESP32),
  never in `core.tick()`.
- A platform `main` calls `hotsync::start_hotsync()` once, after creating the
  bus channel and before `core::setup_transports`: it puts the cradle over the
  line, starts the service and registers `hotsync.*`. Builds without
  `FN_WITH_HOTSYNC` link a stub of it that does nothing.
- NetSync binds its port once the network is up: until then `bind()` fails,
  and the service tries again every 5 s. It does not subscribe to
  `SystemEvents` ([event_driven_services.md](event_driven_services.md)):
  those callbacks run on the publisher's thread, and the service has a task
  of its own, so an event could only set a flag for its loop to poll. The
  retry does that with nothing shared between threads, also recovers a port
  another program holds on POSIX, and lets the cradle, which needs no
  network, start at once.
- `FsStorage` keeps the folders above on an `fs::IFileSystem`.
- HotSync adds no FujiBus device, command or setting on the wire, so
  `py/fujinet_tools` and `fujinet-nio-lib` have nothing to follow.
- Tests: `tests/test_hotsync.cpp` replays bytes captured from a Palm OS 3.3
  device, palm-sync's NetSync recording and the emulator rig, runs whole
  syncs against a fake Palm, and covers the cradle hand-off: app traffic
  holding the line, stray bytes ending the hold, a WAKEUP inside a window, a
  window with none, and the rate restored. It also pins the cradle's packet
  test to the transport's, runs syncs beside another thread's file traffic,
  checks SHA-256 against the FIPS 180-4 examples, and drives
  `start_hotsync()`.
- Not covered by automated tests, but tested live: the ESP32 service task and
  `UartChannel::set_baud_rate` (on an FN-RS232 board with a Palm in a
  cradle), and NetSync with a real Palm (only with `fake_palm.py` and
  palm-sync's recording, since the test Palm has no network).

## Licence

The protocol code in `src/lib/hotsync/` marked "Derived from palm-sync" is
based on [palm-sync](https://github.com/jichu4n/palm-sync) by Chuan Ji,
licensed under the Apache License, Version 2.0. You may obtain a copy of the
licence at <https://www.apache.org/licenses/LICENSE-2.0>. Those files have been
modified from the original: rewritten in C++ for FujiNet NIO. No pilot-link
(GPL) code is used.
