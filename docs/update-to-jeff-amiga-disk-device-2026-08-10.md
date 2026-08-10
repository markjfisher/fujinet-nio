# Amiga DiskDevice implementation update for Jeff

**Status: Draft for discussion (2026-08-10).** This is a point-in-time update
following the archived
[`response-to-jeff-amiga-disk-device-plan.md`](archive/response-to-jeff-amiga-disk-device-plan.md)
and Jeff's `response-to-mark-driver-questions.md`.

The agreements in that exchange have now been taken through the first working
driver implementation.

## Working read-only path

- The Amiga implementation lives under `fujinet-nio-driver/amiga`.
- `fujinet-disk.device` is a native resident Exec device.
- Amiga unit 0 maps to DiskDevice slot 1.
- The first profile is a read-only raw 880 KiB ADF with 512-byte sectors and
  1760 blocks.
- A MountList and CLI mount utility configure `DN0:`.
- The device implements the trackdisk subset needed by the tested AmigaDOS
  path, including geometry, protection, and change-state queries.
- Mount, Info, geometry, and block reads use the typed DiskDevice client in
  `fujinet-nio-lib`.
- AmigaDOS mounts the volume and successfully performs `Dir` and `Type`
  against a deterministic ADF in Amiberry.

## Channel and session result

The shared byte-channel/session layer is implemented, with RS-232 retained as
the initial correctness backend. FujiBus packet and DiskDevice logic remain
independent of SLIP and `serial.device`, allowing a packet-native Pico, Zorro,
or other faster backend to sit beneath the same driver API later.

Capability reporting for packet-native channels remains design work; it was
not required to validate the initial RS-232 path.

## Resident lifecycle and caller ownership

The resident device exposed two lifecycle constraints that are now explicit:

- its dedicated library build does not use application `atexit()` cleanup;
- each serial exchange uses a reply port owned by its current calling task;
- each unit owns explicit packet request, packet response, parser, and codec
  scratch state rather than large caller-stack buffers or legacy global
  DiskDevice state;
- an Exec `SignalSemaphore` serializes callers sharing the physical RS-232
  session;
- a host test interleaves two independent client contexts and verifies state
  isolation; and
- the Amiberry acceptance test launches two independent Amiga CLI processes
  issuing block reads through the resident unit, with both completing
  successfully.

This resolves the application-global transport concern for the DiskDevice
path without changing the existing public API used by 8-bit clients.

## Validation

- Native 68000 driver build and driver contract tests pass.
- The production adapter links against a built `fujinet-nio-lib` archive.
- `fujinet-nio-lib make check` passes for the configured Atari, BBC, MS-DOS,
  Linux, and Amiga targets and host tests.
- The complete workspace Amiberry suite passes.

## Deliberately deferred

- Writes remain disabled until cache ownership, flush ordering, and failure
  semantics are agreed.
- Hot swap remains deferred until NIO media state, driver notification, and
  AmigaDOS cache invalidation are designed together.
- HDF/RDB and larger hard-disk geometry remain a separate profile.
- The native Amiga serial receive path still needs a timer-backed deadline for
  a completely silent peer; its current first-byte fallback can block.
- Pico/native-packet and faster-channel backends follow read-only review.

## Requested review

Review from the Amiga side would be particularly useful for:

- the native Exec and trackdisk command behavior;
- the `DN0` MountList geometry;
- synchronous semaphore serialization versus a future internal unit task;
- running the driver in Jeff's emulator and physical RS-232 harnesses; and
- the preferred larger raw-block geometry when the combined floppy/hard-disk
  direction is ready to progress.
