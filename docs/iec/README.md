# Commodore IEC bus

fujinet-nio on a Commodore 64's serial bus. The design follows the Atari's: no legacy
transport or framing, one reserved device for NIO packets, and a standard drive
frontend that composes `DiskService` directly.

| Layer | Where | What it does |
|---|---|---|
| Program | `fujinet-nio-lib` target `c64`, `nio-config` target `c64` | FujiBus packets through the KERNAL's bus routines |
| Ingress | `NetIecIngress` (`io/netiec_ingress.h`) | Bus transactions from an emulator. On hardware, a line driver takes this place |
| Bus | `IecBus` (`io/iec_bus.h`) | Who is addressed: ATN commands, LISTEN and TALK roles, OPEN names |
| Devices | `IecDriveFrontend`, `IecNioFrontend`, composed by `IecDeviceSet` | Drives 8-15 and the packet channel on device 30 |
| Disk | `CbmVolume` over `DiskService` and the D64 and PRG image types | The DOS structures on a disk, as sectors |

None of the portable layers has a platform `#ifdef`. `IecBus` is the seam a hardware
ingress plugs into.

- [Drive](drive.md): what a program sees of drives 8-15
- [NIO packet channel](nio-channel.md): device 30, the contract for C clients
- [NetIEC](netiec.md): the emulator's link

## Running it

```sh
./build.sh -cp c64-netiec-debug          # the firmware; listens for NetIEC on port 1541
```

VICE needs the `vice-fn` build, whose real-device support reaches units 8-30 through
the `opencbm-netiec` bridge. From the workspace, `scripts/c64-run` starts the firmware
and a windowed VICE, and `integration-tests/c64-iec/run_boot_acceptance.py` drives the
whole path: boot, config app, TNFS, mount, reboot, run a program from the mounted disk.

On the C64, `LOAD"*",8,1` then `RUN` starts the config app from the boot disk. F1 in
the app mounts the assigned images and restarts the machine; the FujiNet keeps its
mounts, so drive 8 is then the image assigned to D1.

## What this has and has not proved

The emulator path goes through VICE's KERNAL traps: a stock KERNAL's serial routines
are hooked, and each makes one request. So it exercises everything above the bus's
electrical behaviour and nothing of the bus's timing. There is no line-level ingress
yet, and nothing here has run on a real C64.

A stock KERNAL is required in the emulator, since the traps check the ROM. JiffyDOS
and fast loaders are out of reach on this path.
